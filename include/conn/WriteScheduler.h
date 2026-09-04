// 写引擎 — 管理本 loop 连接的响应发送队列和部分发送
// 高优先级 TEXT 帧先于低优先级 BINARY 数据发送
// 遇到 EAGAIN 时注册 EPOLLOUT 等可写再发
// 遇到 EPIPE 时回调 del_connection 销毁连接
// 只处理本 loop 连接的写入 跨 loop 投递由 Connection::send 路由到归属 loop 后入队
// 每子 loop 一个 主 loop 无连接不建
#pragma once

#include <queue>
#include <unordered_map>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "Connection.h"
#include "LazyBuffer.h"
#include "../core/EventLoop.h"

class WriteScheduler {
public:
    using DelConnectionFn = std::function<void(const std::shared_ptr<Connection>&)>;

    WriteScheduler(EventLoop& loop, DelConnectionFn del_conn);

    // 本写引擎所在事件循环 归属线程判定与跨线程投递目标
    EventLoop& loop() { return this->loop_; }

    // 把一帧响应入本 loop 高/低队列并立即排空 只能在本 loop 线程调用
    void enqueue(const std::shared_ptr<Connection>& conn,
                 std::string data, bool is_high_priority);
    // 写事件回调
    void handle_write(const std::shared_ptr<Connection>& conn);
    // 移除指定连接的未完成写入 连接销毁时调用
    void remove_pending(const std::shared_ptr<Connection>& conn);

private:
    // 本地待响应的结构体（发送目标+发送内容）
    struct PendingResponse {
        std::shared_ptr<Connection> conn_;
        std::string data_;
    };
    // 先排空高优先队列再低优先队列
    void drain_all();
    // 发送一个队列
    void drain_one(std::queue<PendingResponse>& q);
    // 发送循环
    std::pair<ssize_t, bool> try_send(int fd, std::string_view view);

    EventLoop& loop_;
    DelConnectionFn del_connection_;

    std::queue<PendingResponse> queue_high_;
    std::queue<PendingResponse> queue_low_;
    std::unordered_map<std::shared_ptr<Connection>, LazyBuffer> pending_writes_;
};
