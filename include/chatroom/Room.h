// 聊天室 — 管理房间内连接集合
// 成员的增删与过期弱引用清理只从 RoomManager 的 join/leave 进 故为私有
// 昵称归房间持有 它随成员资格生灭 会话退出后不该还挂着上次进房的昵称
// 广播方只取成员快照 get_connections，不自己判断成员是否重复或过期
//
// 锁序：RoomManager::mtx_ 与 Room::mtx_ 是唯一的嵌套方向，Room::mtx_
// 与 TransferManager::mtx_ 从不嵌套，持锁期间只调 Room 自己的方法

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
    // 成员快照 弱引用已转活 昵称已拷出 锁外随便用
    struct Member {
        std::shared_ptr<Session> sess_;   // 寻址用
        std::string nick_;                // 进房那一刻定下 此后不动
    };

    // 房间人数上限，满员时 add_num 拒绝
    static constexpr size_t kMaxMembers = 9;

    // 取成员列表快照，只保证 Session 对象还在，不管连接是否已关闭
    std::vector<Member> get_connections();
    // 房间内的文件传输管理器，返回引用期间须由调用方持住本房间的 shared_ptr
    TransferManager& transfer_mgr() { return transfer_mgr_; }

private:
    friend class RoomManager;

    // 成员条目 弱引用不吊住连接 昵称随这条entry一起进出
    struct Entry {
        std::weak_ptr<Session> sess_;
        std::string nick_;
    };

    // 加入房间，满员返回 false
    // 入口顺带清掉过期条目，扫描规模只与真实成员数有关
    bool add_num(const std::shared_ptr<Session>& sess, std::string nick);
    // 移除连接
    void del_num(const std::shared_ptr<Session>& sess);
    // 房间是否已无成员，按弱引用能否转活判断
    bool empty();

    std::vector<Entry> connections_;                        // 房间成员
    TransferManager transfer_mgr_;                          // 传输管理器
    std::shared_mutex mtx_;                                 // 成员列表锁
};

// join_room 返回值，无效时不建房间也不留成员
struct JoinResult {
    bool valid_ = false;                                    // 加入成功
    std::vector<Room::Member> members_;                     // 加入后的成员快照含自己
};

// 房间管理器
class RoomManager {
public:
    // 加入房间，查不到即建，容量检查与成员快照在同一把锁下完成
    // 满员返回无效结果，房间与成员都不变
    JoinResult join_room(const std::string& room_id, const std::shared_ptr<Session>& sess,
                         std::string nick);

    // 只查不建，房间不存在返回 nullptr
    // 广播、信令、转移查询都走这个，避免向不存在的房间发消息时留下空房间
    // 返回的 shared_ptr 是房间存活凭据，调用方须持到用完为止
    std::shared_ptr<Room> find_room(const std::string& room_id);

    // 从房间移除连接，空房间自动清理，返回剩余成员列表
    std::vector<Room::Member> leave_room(const std::string& room_id,
                                         const std::shared_ptr<Session>& sess);

private:
    std::unordered_map<std::string, std::shared_ptr<Room>> rooms_;
    // 映射锁
    std::shared_mutex mtx_;
};
