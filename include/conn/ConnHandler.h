// ConnHandler — 连接生命周期管理器 一个 EventLoop 独立处理连接的最小单元
// 持有写调度和两个协议处理器 主从 Reactor 模型下每个从属事件循环实例化一份
// 连接所有权由事件循环回调捕获的 shared_ptr 持有 本类只负责注册销毁
#pragma once

#include <memory>
#include <functional>

#include "Connection.h"
#include "WriteScheduler.h"
#include "../http/HttpHandler.h"
#include "../ws/WsHandler.h"
#include "../core/EventLoop.h"
#include "../core/ThreadPool.h"
#include "../chatroom/Room.h"

class ConnHandler {
public:
    // 共享资源由外部传入 事件循环本类持有 线程池仅转交给协议处理器
    ConnHandler(EventLoop& loop, ThreadPool& works,
                RoomManager& room_mgr);

    // 添加一个客户端连接到本处理器
    // 必须在 IO 线程调用 直接注册到 EventLoop
    void add_connection(int fd);

    // 唯一的连接关闭入口 通用销毁 + 按 ws_mode_ 分发协议清理
    // 必须在 IO 线程调用
    void close_connection(const std::shared_ptr<Connection>& conn);

private:
    // IO 处理
    bool read_data(const std::shared_ptr<Connection>& conn);
    void handle_clientfd(const std::shared_ptr<Connection>& conn);

    // 协议无关的连接销毁 只被 close_connection 调用
    void del_connection(const std::shared_ptr<Connection>& conn);

    EventLoop& loop_;
    WriteScheduler writer_;
    HttpHandler http_handler_;
    WsHandler ws_handler_;

    static constexpr int BUFFER_SIZE = 4096;
};
