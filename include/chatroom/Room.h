// 聊天室 — 管理房间内连接集合

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

class Connection;

class Room {
public:
    // 添加连接
    void add_num(const std::shared_ptr<Connection>& conn);
    // 移除连接
    void del_num(const std::shared_ptr<Connection>& conn);
    // 获取当前在线连接列表
    std::vector<std::shared_ptr<Connection>> get_live_connections();

private:
    std::vector<std::weak_ptr<Connection>> connections_;
    std::mutex mtx_;
};

// 房间管理器
class RoomManager {
public:
    // 获取或创建房间
    std::shared_ptr<Room> get_or_create(const std::string& room_id);

    // 从房间移除连接，空房间自动清理，返回剩余成员列表
    std::vector<std::shared_ptr<Connection>> leave_room(
        const std::string& room_id,
        const std::shared_ptr<Connection>& conn);

private:
    std::unordered_map<std::string, std::shared_ptr<Room>> rooms_;
    std::mutex mtx_;
};
