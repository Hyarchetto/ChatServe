// 聊天室 & 房间管理器实现

#include "chatroom/Room.h"
#include "ctrl/Session.h"

#include <algorithm>
#include <mutex>

// ==================== Room ====================

bool Room::add_num(const std::shared_ptr<Session>& sess) {
    std::unique_lock<std::shared_mutex> lock(this->mtx_);
    // 清过期弱引用，列表规模始终跟着真实成员走
    this->connections_.erase(
        std::remove_if(this->connections_.begin(), this->connections_.end(),
            [](const std::weak_ptr<Session>& wp) { return wp.expired(); }),
            this->connections_.end());
    // 满员则拒绝
    if (this->connections_.size() >= kMaxMembers) {
        return false;
    }
    this->connections_.push_back(sess);
    return true;
}

void Room::del_num(const std::shared_ptr<Session>& sess) {
    std::unique_lock<std::shared_mutex> lock(this->mtx_);
    this->connections_.erase(
        std::remove_if(this->connections_.begin(), this->connections_.end(),
            [&sess](const std::weak_ptr<Session>& wp) {
                auto sp = wp.lock();
                return !sp || sp == sess;
            }), this->connections_.end());
}

std::vector<std::shared_ptr<Session>> Room::get_connections() {
    // 读多写少 广播并发读共享锁
    std::shared_lock<std::shared_mutex> lock(this->mtx_);
    std::vector<std::shared_ptr<Session>> members;
    for (auto& wp : this->connections_) {
        if (auto sp = wp.lock()) {
            members.push_back(sp);
        }
    }
    return members;
}

bool Room::empty() {
    std::shared_lock<std::shared_mutex> lock(this->mtx_);
    // 有一个成员还在就不回收，无需建整份快照
    for (auto& wp : this->connections_) {
        if (wp.lock()) {
            return false;
        }
    }
    return true;
}

// ==================== RoomManager ====================
// 获取房间
std::shared_ptr<Room> RoomManager::get_or_create(const std::string& room_id) {
    if (auto room = this->find_room(room_id)) {
        return room;
    }
    // 没有对应房间，创建新房间
    auto room = std::make_shared<Room>();
    {
        // 上写锁创建新房间
        std::unique_lock lock(this->mtx_);
        auto it = this->rooms_.find(room_id);
        if (it != this->rooms_.end()) {
            return it->second;
        }
        this->rooms_[room_id] = room;
        return room;
    }
}
// 查询房间
std::shared_ptr<Room> RoomManager::find_room(const std::string& room_id) {
    // 上读锁查找
    std::shared_lock lock(this->mtx_);
    auto it = this->rooms_.find(room_id);
    if (it == this->rooms_.end()) {
        return nullptr;
    }
    return it->second;
}
// 离开房间
std::vector<std::shared_ptr<Session>> RoomManager::leave_room(const std::string& room_id,
                                                              const std::shared_ptr<Session>& sess) {
    auto room = this->find_room(room_id);
    if (!room) {
        return {};
    }

    room->del_num(sess);
    auto members = room->get_connections();
    // 房间空则回收 唯一锁下复查 防等待期间新成员加入或房间已被重建导致误删
    if (members.empty()) {
        std::unique_lock lock(this->mtx_);
        auto it = this->rooms_.find(room_id);
        if (it != this->rooms_.end() && it->second == room &&
            it->second->empty()) {
            this->rooms_.erase(it);
        }
    }
    return members;
}
