// HttpHandler — HTTP 协议处理器
// 收到完整 HTTP 请求后丢线程池路由，结果通过 EventLoop 投回 IO 线程发送
// WebSocket 升级请求在此完成 101 响应，之后 conn->ws_mode_ 标记为 true
#pragma once

#include <memory>
#include <functional>

#include "../conn/Connection.h"
#include "../core/EventLoop.h"
#include "../core/ThreadPool.h"
#include "HttpRouter.h"
#include "../conn/WriteScheduler.h"

class HttpHandler {
public:
    HttpHandler(EventLoop& loop, ThreadPool& works, WriteScheduler& writer);

    void handle_http(const std::shared_ptr<Connection>& conn);

private:
    EventLoop& loop_;
    ThreadPool& works_;
    // 路由只被本类消费 内部持有 路由表构造时固定 运行期只读
    HttpRouter http_router_;
    WriteScheduler& writer_;
};
