// 聊天室 & 房间管理器实现

#include "chatroom/Room.h"
#include "server/Connection.h"

// ==================== Room ====================

void Room::add_num(const std::shared_ptr<Connection>& conn) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    // 清理过期弱引用
    this->connections_.erase(
        std::remove_if(this->connections_.begin(), this->connections_.end(),
            [](const std::weak_ptr<Connection>& wp) { return wp.expired(); }),
        this->connections_.end());
    this->connections_.push_back(conn);
}

void Room::del_num(const std::shared_ptr<Connection>& conn) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    this->connections_.erase(
        std::remove_if(this->connections_.begin(), this->connections_.end(),
            [&conn](const std::weak_ptr<Connection>& wp) {
                auto sp = wp.lock();
                return !sp || sp == conn;
            }),
        this->connections_.end());
}

std::vector<std::shared_ptr<Connection>> Room::get_live_connections() {
    std::lock_guard<std::mutex> lock(this->mtx_);
    std::vector<std::shared_ptr<Connection>> live;
    for (auto& wp : this->connections_) {
        if (auto sp = wp.lock()) {
            live.push_back(sp);
        }
    }
    return live;
}

// ==================== RoomManager ====================

std::shared_ptr<Room> RoomManager::get_or_create(const std::string& room_id) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    auto it = this->rooms_.find(room_id);
    if (it != this->rooms_.end()) {
        return it->second;
    }
    auto room = std::make_shared<Room>();
    this->rooms_[room_id] = room;
    return room;
}
std::vector<std::shared_ptr<Connection>> RoomManager::leave_room(
    const std::string& room_id,
    const std::shared_ptr<Connection>& conn) {
    std::lock_guard<std::mutex> lock(this->mtx_);

    auto it = this->rooms_.find(room_id);
    if (it == this->rooms_.end()) {
        return {};
    }
    
    it->second->del_num(conn);

    auto live = it->second->get_live_connections();
    if (live.empty()) {
        this->rooms_.erase(it);
    }
    return live;
}
