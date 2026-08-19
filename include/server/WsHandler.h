// WsHandler — WebSocket 协议处理器
// TEXT 走 WsAppRouter 路由，BINARY 走 TransferManager 滑动窗口转发
// CLOSE 帧和意外断开时清理传输和房间状态并广播通知
#pragma once

#include <memory>

#include "Connection.h"
#include "ConnRegistry.h"
#include "WriteScheduler.h"
#include "../core/EventLoop.h"
#include "../core/ThreadPool.h"
#include "../ws/WsAppRouter.h"
#include "../chatroom/Room.h"
#include "../transfer/TransferManager.h"

class WsHandler {
public:
    WsHandler(EventLoop& loop, ThreadPool& works,
                 WsAppRouter& ws_app_router,
                 RoomManager& room_mgr,
                 TransferManager& transfer_mgr,
                 ConnRegistry& conn_registry,
                 WriteScheduler& writer);

    void handle_ws(const std::shared_ptr<Connection>& conn);
    // WS 清理 取消传输离开房间并广播
    void cleanup(const std::shared_ptr<Connection>& conn);

private:
    EventLoop& loop_;
    ThreadPool& works_;
    WsAppRouter& ws_app_router_;
    RoomManager& room_mgr_;
    TransferManager& transfer_mgr_;
    ConnRegistry& conn_registry_;
    WriteScheduler& writer_;
};
