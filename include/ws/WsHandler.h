// WsHandler — WebSocket 协议处理器
// TEXT 走 WsAppRouter 路由，BINARY 走 TransferManager 滑动窗口转发
// CLOSE 帧和意外断开时清理传输和房间状态并广播通知
#pragma once

#include <memory>

#include "../conn/Connection.h"
#include "../conn/WriteScheduler.h"
#include "../core/EventLoop.h"
#include "../core/ThreadPool.h"
#include "WsAppRouter.h"
#include "../chatroom/Room.h"

class TransferManager;

class WsHandler {
public:
    WsHandler(EventLoop& loop, ThreadPool& works,
                 RoomManager& room_mgr,
                 WriteScheduler& writer);

    void handle_ws(const std::shared_ptr<Connection>& conn);
    // WS 清理 取消传输离开房间并广播
    void cleanup(const std::shared_ptr<Connection>& conn);

private:
    // 取 conn 所在房间的文件传输管理器 传输状态归房间 每个房间独立
    TransferManager& transfer_mgr_of(const std::shared_ptr<Connection>& conn);

    EventLoop& loop_;
    ThreadPool& works_;
    // 应用层路由只被本类消费 内部持有 路由表构造时固定 运行期只读
    WsAppRouter ws_app_router_;
    RoomManager& room_mgr_;
    WriteScheduler& writer_;
};
