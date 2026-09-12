// 应用层命令路由 — 聊天/信令/文件传输三条命令域共用一张命令表
// 跑在业务线程池或中控持锁调用 只认 Session 控制块与 RoomManager
// 输出按 Session 寻址的应用文本 组帧由 io 侧完成
// 表机制与共享助手在本类 三条命令域各自占一个 cpp
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
    // 输出一条按 Session 寻址的应用文本
    using Out = CtrlDown;
    // 处理器 持锁调用 房间/传输状态经 RoomManager 取用
    using Handler = std::function<std::vector<Out>(
        const AppMessage& msg,
        std::shared_ptr<Session> session,
        RoomManager& room_mgr)>;

    AppRouter();

    // 分发命令 返回待发帧 未识别命令与裸文本按聊天兜底(与旧一致)
    std::vector<Out> handle(const AppMessage& msg,
                            std::shared_ptr<Session> session,
                            RoomManager& room_mgr);

    // 断开清理 取消房间内传输并离开房间 返回发给剩余成员的帧
    std::vector<Out> cleanup(std::shared_ptr<Session> session,
                             RoomManager& room_mgr);

    // 处理上传方 BINARY 分块 定位会话并转发给下载方 与旧转发逻辑一致
    std::vector<Out> handle_chunk(std::shared_ptr<Session> session,
                                  const std::string& data,
                                  RoomManager& room_mgr);

private:
    // 注册命令处理器
    void on(const std::string& command, Handler handler);
    // 注册默认处理器 裸文本或未识别命令
    void on_default(Handler handler);

    // 三条命令域各自注册处理器 分别定义在各自 cpp
    void register_chat();
    void register_signalling();
    void register_transfer();

    // 广播帧给 live 列表里除 except 外的所有 Session
    static void broadcast_except(
        const std::vector<std::shared_ptr<Session>>& live, Session* except,
        std::vector<Out>& results, const std::string& text);
    // 向某房间广播一帧给除 except 外的成员 结果追加进 results
    static void broadcast_to_room(RoomManager& room_mgr,
                                  const std::string& room_id, Session* except,
                                  const std::string& text,
                                  std::vector<Out>& results);
    // 房间的文件传输管理器 传输状态归房间
    static TransferManager& transfer_mgr_of(RoomManager& room_mgr,
                                            const std::string& room_id);
    // MEMBERS 列表文本 fd:nick 逗号分隔 客户端昵称表据此建立
    static std::string members_frame(
        const std::vector<std::shared_ptr<Session>>& live);
    // 一条 DWERR 文本 寻址由调用点决定
    static std::string dwerr_frame(const std::string& file_id,
                                   const std::string& reason);

    std::unordered_map<std::string, Handler> handlers_;
    Handler default_handler_;
};
