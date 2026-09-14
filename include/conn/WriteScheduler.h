// 写引擎 — 管理本 loop 连接的响应发送队列和部分发送
// 高优先级 TEXT 帧先于低优先级 BINARY 数据发送
// 遇到 EAGAIN 时注册 EPOLLOUT 等可写再发
// 遇到 EPIPE 时回调 del_connection 销毁连接
// 冲刷后关闭归本引擎自持 调用方只表达意图 不碰连接状态
// 只处理本 loop 连接的写入 发送全在本 loop 线程发生 无跨线程投递
// 每子 loop 一个 主 loop 无连接不建
#pragma once

#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <sys/types.h>

#include "Connection.h"
#include "LazyBuffer.h"
#include "../core/EventLoop.h"

class WriteScheduler {
public:
    using DelConnectionFn = std::function<void(const std::shared_ptr<Connection>&)>;

    // 单连接待写字节上限，慢客户端跟不上时缓冲会一直涨
    // 超过就不再接收新帧直接断开，丢弃会让对端卡在等不来的数据上
    static constexpr size_t kMaxPendingBytes = 16 * 1024 * 1024;

    WriteScheduler(EventLoop& loop, DelConnectionFn del_conn);

    // 本写引擎所在事件循环 归属线程判定与跨线程投递目标
    EventLoop& loop() { return this->loop_; }

    // 把一帧响应入本 loop 高/低队列并立即排空 只能在本 loop 线程调用
    void enqueue(const std::shared_ptr<Connection>& conn,
                 std::string data, bool is_high_priority);
    // 写事件回调
    void handle_write(const std::shared_ptr<Connection>& conn);
    // 冲刷后关闭 — 待写数据发完再回调 del_connection 缓冲已空则立即收
    void request_close(const std::shared_ptr<Connection>& conn);
    // 移除指定连接的未完成写入 连接销毁时调用
    void remove_pending(const std::shared_ptr<Connection>& conn);

private:
    // 本地待响应的结构体 发送目标与发送内容
    struct PendingResponse {
        std::shared_ptr<Connection> conn_;
        std::string data_;
    };
    // 先排空高优先队列再低优先队列
    void drain_all();
    // 发送一个队列
    void drain_one(std::queue<PendingResponse>& q);
    // 该连接待写缓冲的字节数，无缓冲为 0
    size_t pending_bytes(const std::shared_ptr<Connection>& conn) const;
    // 发送循环
    std::pair<ssize_t, bool> try_send(int fd, std::string_view view);

    EventLoop& loop_;
    DelConnectionFn del_connection_;

    std::queue<PendingResponse> queue_high_;
    std::queue<PendingResponse> queue_low_;
    std::unordered_map<std::shared_ptr<Connection>, LazyBuffer> pending_writes_;
    // 已请求冲刷后关闭的连接 缓冲发空即回调
    std::unordered_set<std::shared_ptr<Connection>> closing_;
};
