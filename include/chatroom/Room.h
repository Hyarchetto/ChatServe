// 聊天室 — 管理房间内连接集合

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "TransferManager.h"

class Session;

class Room {
public:
    // 添加连接
    void add_num(const std::shared_ptr<Session>& conn);
    // 移除连接
    void del_num(const std::shared_ptr<Session>& conn);
    // 获取当前在线连接列表
    std::vector<std::shared_ptr<Session>> get_live_connections();
    // 房间内的文件传输管理器
    TransferManager& transfer_mgr() { return transfer_mgr_; }

private:
    std::vector<std::weak_ptr<Session>> connections_;
    TransferManager transfer_mgr_;
    std::shared_mutex mtx_;    // 成员列表锁
};

// 房间管理器
class RoomManager {
public:
    // 获取或创建房间
    std::shared_ptr<Room> get_or_create(const std::string& room_id);

    // 从房间移除连接，空房间自动清理，返回剩余成员列表
    std::vector<std::shared_ptr<Session>> leave_room(
        const std::string& room_id,
        const std::shared_ptr<Session>& conn);

private:
    std::unordered_map<std::string, std::shared_ptr<Room>> rooms_;
    // 映射锁
    std::shared_mutex mtx_;
};
