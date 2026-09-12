// 聊天室 & 房间管理器实现

#include "chatroom/Room.h"
#include "ctrl/Session.h"

#include <algorithm>

// ==================== Room ====================

void Room::add_num(const std::shared_ptr<Session>& conn) {
    std::unique_lock<std::shared_mutex> lock(this->mtx_);
    // 清理过期弱引用
    this->connections_.erase(
        std::remove_if(this->connections_.begin(), this->connections_.end(),
            [](const std::weak_ptr<Session>& wp) { return wp.expired(); }),
        this->connections_.end());
    this->connections_.push_back(conn);
}

void Room::del_num(const std::shared_ptr<Session>& conn) {
    std::unique_lock<std::shared_mutex> lock(this->mtx_);
    this->connections_.erase(
        std::remove_if(this->connections_.begin(), this->connections_.end(),
            [&conn](const std::weak_ptr<Session>& wp) {
                auto sp = wp.lock();
                return !sp || sp == conn;
            }),
        this->connections_.end());
}

std::vector<std::shared_ptr<Session>> Room::get_live_connections() {
    // 读多写少 广播并发读共享锁
    std::shared_lock<std::shared_mutex> lock(this->mtx_);
    std::vector<std::shared_ptr<Session>> live;
    for (auto& wp : this->connections_) {
        if (auto sp = wp.lock()) {
            live.push_back(sp);
        }
    }
    return live;
}

// ==================== RoomManager ====================

std::shared_ptr<Room> RoomManager::get_or_create(const std::string& room_id) {
    {
        std::shared_lock lock(this->mtx_);
        auto it = this->rooms_.find(room_id);
        if (it != this->rooms_.end()) {
            return it->second;
        }
    }
    // 共享锁未命中 先造房间再唯一锁双检插入 防共享锁释放期间另一线程已插入
    auto room = std::make_shared<Room>();
    {
        std::unique_lock lock(this->mtx_);
        auto it = this->rooms_.find(room_id);
        if (it != this->rooms_.end()) {
            return it->second;
        }
        this->rooms_[room_id] = room;
        return room;
    }
}

std::vector<std::shared_ptr<Session>> RoomManager::leave_room(
    const std::string& room_id,
    const std::shared_ptr<Session>& conn) {
    // 共享锁只取房间引用 房间操作在房间自身锁内进行 不在管理器锁上停留
    std::shared_ptr<Room> room;
    {
        std::shared_lock lock(this->mtx_);
        auto it = this->rooms_.find(room_id);
        if (it == this->rooms_.end()) {
            return {};
        }
        room = it->second;
    }

    room->del_num(conn);
    auto live = room->get_live_connections();
    // 房间空则回收 唯一锁下复查 防等待期间新成员加入或房间已被重建导致误删
    if (live.empty()) {
        std::unique_lock lock(this->mtx_);
        auto it = this->rooms_.find(room_id);
        if (it != this->rooms_.end() && it->second == room &&
            it->second->get_live_connections().empty()) {
            this->rooms_.erase(it);
        }
    }
    return live;
}
