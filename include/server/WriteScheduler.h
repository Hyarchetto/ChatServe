// 写调度器 — 管理响应发送队列和部分发送
// 高优先级 TEXT 帧先于低优先级 BINARY 数据发送
// 遇到 EAGAIN 时注册 EPOLLOUT 等可写再发
// 遇到 EPIPE 时回调 del_connection 销毁连接
#pragma once

#include <queue>
#include <unordered_map>
#include <functional>
#include <memory>
#include <string>

#include "Connection.h"
#include "../core/EventLoop.h"

class WriteScheduler {
public:
    using DelConnectionFn = std::function<void(const std::shared_ptr<Connection>&)>;

    WriteScheduler(EventLoop& loop, DelConnectionFn del_conn);

    void push_response(const std::shared_ptr<Connection>& conn,
                       std::string data, bool is_high_priority);
    void flush_responses();
    void handle_write(const std::shared_ptr<Connection>& conn);

    // 移除指定连接的未完成写入 连接销毁时调用
    void remove_pending(const std::shared_ptr<Connection>& conn);

private:
    struct PendingResponse {
        std::shared_ptr<Connection> conn_;
        std::string data_;
    };

    void drain_one(std::queue<PendingResponse>& q);

    EventLoop& loop_;
    DelConnectionFn del_connection_;

    std::queue<PendingResponse> queue_high_;
    std::queue<PendingResponse> queue_low_;
    std::unordered_map<std::shared_ptr<Connection>, std::string> pending_writes_;
    bool flushing_ = false;
};
