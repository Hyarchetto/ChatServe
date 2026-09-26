// 应用层命令路由 — 聊天/信令/文件传输/心跳四条命令域共用一张命令表
// 跑在业务线程池 只认 Session 控制块与持有的 RoomManager
// 输出按 Session 寻址的应用文本 组帧由 io 侧完成
// 同一 Session 的命令与收尾由中控单飞门串行 类内不需要串行锁
// 房间由调用方在栈上持 shared_ptr 存活 传输管理器随房间走
// 表机制与共享助手在本类 每个命令域各自占一个 cpp
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../ctrl/Session.h"
#include "../ctrl/CtrlMsg.h"
#include "../chatroom/Room.h"
#include "AppMessage.h"

class AppRouter {
public:
    // 处理器 房间/传输状态经 room_mgr_ 取用
    using Handler = std::function<std::vector<CtrlDown>(std::shared_ptr<Session> sess,
                                                        const AppMessage& msg)>;

    // room_mgr 构造时注入 全程只认这一个房间容器 生命周期由调用方保证长于本对象
    explicit AppRouter(RoomManager& room_mgr);

    // 分发命令 返回待发帧 未识别的命令与裸文本丢弃
    std::vector<CtrlDown> handle(std::shared_ptr<Session> sess, const AppMessage& msg);

    // 断开清理 取消房间内传输并离开房间 返回发给剩余成员的帧
    std::vector<CtrlDown> cleanup(std::shared_ptr<Session> sess);

    // 处理上传方 BINARY 分块 定位会话并转发给下载方
    std::vector<CtrlDown> handle_chunk(std::shared_ptr<Session> sess, const std::string& data);

private:
    // 注册命令处理器
    void on(const std::string& command, Handler handler);

    // 每个命令域各自注册处理器
    void register_chat();
    void register_signalling();
    void register_transfer();
    void register_heartbeat();

    // 广播帧给成员快照里除 except 外的所有 Session
    static void broadcast_except(const std::vector<Room::Member>& live, Session* except,
                                 const std::string& text, std::vector<CtrlDown>& results);
    // 广播帧给房间里除 except 外的所有 Session，房间为空直接弃包
    static void broadcast_to_room(const std::shared_ptr<Room>& room, Session* except,
                                  const std::string& text, std::vector<CtrlDown>& results);
    // 房间内按 fd 找目标 Session 房间为空即无对象可发
    static std::shared_ptr<Session> find_peer(const std::shared_ptr<Room>& room, int target_fd);
    // 解析 target_fd 并把 payload 转发给同房间的目标连接 帧里换发送方为自己的 fd
    std::vector<CtrlDown> relay_signal(std::shared_ptr<Session> sess, const AppMessage& msg,
                                       const std::string& command, size_t payload_count);
    // MEMBERS 列表文本 fd:nick 逗号分隔 客户端昵称表据此建立
    static std::string build_members_frame(const std::vector<Room::Member>& live);
    // 上传方断线导致的传输失败文案
    static constexpr char kUploaderGone[] = "上传方已离开，下载失败";
    // 一条 DWERR 文本 寻址由调用点决定
    static std::string build_dwerr_frame(const std::string& file_id, const std::string& reason);

    RoomManager& room_mgr_;
    std::unordered_map<std::string, Handler> handlers_;
};
