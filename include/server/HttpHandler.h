// HttpHandler — HTTP 协议处理器
// 收到完整 HTTP 请求后丢线程池路由，结果通过 EventLoop 投回 IO 线程发送
// WebSocket 升级请求在此完成 101 响应，之后 conn->ws_mode_ 标记为 true
#pragma once

#include <memory>
#include <functional>

#include "Connection.h"
#include "../core/EventLoop.h"
#include "../core/ThreadPool.h"
#include "../http/HttpRouter.h"
#include "WriteScheduler.h"

class HttpHandler {
public:
    using CloseConnectionFn = std::function<void(const std::shared_ptr<Connection>&)>;

    HttpHandler(EventLoop& loop, ThreadPool& works,
                   HttpRouter& http_router, WriteScheduler& writer,
                   CloseConnectionFn close_conn);

    void handle_http(const std::shared_ptr<Connection>& conn);

private:
    EventLoop& loop_;
    ThreadPool& works_;
    HttpRouter& http_router_;
    WriteScheduler& writer_;
    CloseConnectionFn close_connection_;
};
