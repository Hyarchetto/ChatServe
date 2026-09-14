// 聊天室 — 管理房间内连接集合
// 成员集合与转移的弱引用清理、人数上限全收在 add_num 一个入口
// 广播方只取成员快照 get_connections，不自己判断成员是否重复或过期

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include "../ctrl/Session.h"
#include "TransferManager.h"

class Room {
public:
    // 房间人数上限，满员时 add_num 拒绝
    static constexpr size_t kMaxMembers = 9;

    // 加入房间，满员返回 false，调用方据此回复客户端
    // 入口顺带清掉过期弱引用，扫描规模只与真实成员数有关
    bool add_num(const std::shared_ptr<Session>& sess);
    // 移除连接
    void del_num(const std::shared_ptr<Session>& sess);
    // 取成员列表快照，只保证 Session 对象还在，不管连接是否已关闭
    std::vector<std::shared_ptr<Session>> get_connections();
    // 房间是否已无成员，按弱引用能否转活判断
    bool empty();
    // 房间内的文件传输管理器
    TransferManager& transfer_mgr() { return transfer_mgr_; }

private:
    std::vector<std::weak_ptr<Session>> connections_;       // 房间成员
    TransferManager transfer_mgr_;                          // 传输管理器
    std::shared_mutex mtx_;                                 // 成员列表锁
};

// 房间管理器
class RoomManager {
public:
    // 获取或创建房间，只有加入房间这一步允许凭空造房
    std::shared_ptr<Room> get_or_create(const std::string& room_id);
    // 只查不建，房间不存在返回 nullptr
    // 广播、信令、转移查询都走这个，避免向不存在的房间发消息时留下空房间
    std::shared_ptr<Room> find_room(const std::string& room_id);

    // 从房间移除连接，空房间自动清理，返回剩余成员列表
    std::vector<std::shared_ptr<Session>> leave_room(const std::string& room_id,
                                                     const std::shared_ptr<Session>& sess);

private:
    std::unordered_map<std::string, std::shared_ptr<Room>> rooms_;
    // 映射锁
    std::shared_mutex mtx_;
};
