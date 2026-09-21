// 聊天室 & 房间管理器实现

#include "chatroom/Room.h"
#include "ctrl/Session.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// ==================== Room ====================

std::vector<Room::Member> Room::get_connections() {
    // 读多写少 广播并发读共享锁
    std::shared_lock<std::shared_mutex> lock(this->mtx_);
    std::vector<Member> members;
    for (auto& e : this->connections_) {
        if (auto sp = e.sess_.lock()) {
            members.push_back({std::move(sp), e.nick_});
        }
    }
    return members;
}

bool Room::add_num(const std::shared_ptr<Session>& sess, std::string nick) {
    std::unique_lock<std::shared_mutex> lock(this->mtx_);
    // 清过期条目，列表规模始终跟着真实成员走
    this->connections_.erase(
        std::remove_if(this->connections_.begin(), this->connections_.end(),
            [](const Entry& e) { return e.sess_.expired(); }),
            this->connections_.end());
    // 满员则拒绝
    if (this->connections_.size() >= kMaxMembers) {
        return false;
    }
    this->connections_.push_back({sess, std::move(nick)});
    return true;
}

void Room::del_num(const std::shared_ptr<Session>& sess) {
    std::unique_lock<std::shared_mutex> lock(this->mtx_);
    this->connections_.erase(
        std::remove_if(this->connections_.begin(), this->connections_.end(),
            [&sess](const Entry& e) {
                auto sp = e.sess_.lock();
                return !sp || sp == sess;
            }), this->connections_.end());
}

bool Room::empty() {
    std::shared_lock<std::shared_mutex> lock(this->mtx_);
    // 有一个成员还在就不回收，无需建整份快照
    for (auto& e : this->connections_) {
        if (!e.sess_.expired()) {
            return false;
        }
    }
    return true;
}

// ==================== RoomManager ====================

// 全程持映射写锁，容量检查与成员快照之间房间不会被另一条线程离开并回收
JoinResult RoomManager::join_room(const std::string& room_id,
                                  const std::shared_ptr<Session>& sess,
                                  std::string nick) {
    JoinResult result;
    std::unique_lock lock(this->mtx_);
    auto it = this->rooms_.find(room_id);
    bool created = false;
    if (it == this->rooms_.end()) {
        it = this->rooms_.emplace(room_id, std::make_shared<Room>()).first;
        created = true;
    }
    Room& room = *it->second;
    if (!room.add_num(sess, std::move(nick))) {
        // 现造的房间没人进得去就地回收 不留空房
        if (created) {
            this->rooms_.erase(it);
        }
        return result;
    }
    result.valid_ = true;
    result.members_ = room.get_connections();
    return result;
}

std::shared_ptr<Room> RoomManager::find_room(const std::string& room_id) {
    std::shared_lock lock(this->mtx_);
    auto it = this->rooms_.find(room_id);
    if (it == this->rooms_.end()) {
        return nullptr;
    }
    return it->second;
}

// 全程持映射写锁，成员移除与空房回收之间房间不会被另一条线程加入回来
// 故不必在回收前复查成员是否真的空了
std::vector<Room::Member> RoomManager::leave_room(const std::string& room_id,
                                                  const std::shared_ptr<Session>& sess) {
    std::unique_lock lock(this->mtx_);
    auto it = this->rooms_.find(room_id);
    if (it == this->rooms_.end()) {
        return {};
    }
    Room& room = *it->second;
    room.del_num(sess);
    auto members = room.get_connections();
    if (members.empty()) {
        this->rooms_.erase(it);
    }
    return members;
}
