// 写引擎 — 本 loop 响应发送队列和部分发送管理
#include "conn/WriteScheduler.h"
#include "core/EventLoop.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

WriteScheduler::WriteScheduler(EventLoop& loop, DelConnectionFn del_conn)
    : loop_(loop), del_connection_(std::move(del_conn)) {}

// 入队高/低队列后立即排空 只由本 loop 归属线程调用
void WriteScheduler::enqueue(const std::shared_ptr<Connection>& conn,
                             std::string data, bool is_high_priority) {
    if (is_high_priority) {
        this->queue_high_.emplace(PendingResponse{conn, std::move(data)});
    }
    else {
        this->queue_low_.emplace(PendingResponse{conn, std::move(data)});
    }
    this->drain_all();
}

void WriteScheduler::handle_write(const std::shared_ptr<Connection>& conn) {
    int fd = conn->sess_->fd_;
    // 缓冲已超上限 先收，避免每次可写事件重复尝试一条永远追不上的连接
    if (this->pending_bytes(conn) > kMaxPendingBytes) {
        this->del_connection_(conn);
        return;
    }
    auto it = this->pending_writes_.find(conn);
    if (it == this->pending_writes_.end()) {
        return;
    }
    LazyBuffer& buf = it->second;
    auto [sent, failed] = this->try_send(fd, std::string_view(buf.data(), buf.size()));

    if (failed) {
        this->pending_writes_.erase(conn);
        this->del_connection_(conn);
        return;
    }

    if (sent >= static_cast<ssize_t>(buf.size())) {
        this->pending_writes_.erase(conn);
        // 连接还存活才删除写事件
        if (conn->sess_->alive_) {
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET);
        }
        // 缓冲发空 请求过冲刷后关闭的在此收
        if (this->closing_.count(conn) > 0) {
            this->del_connection_(conn);
        }
    }
    else {
        buf.consume(sent);
    }
}

// 冲刷后关闭 — 待写数据发完再回调 del_connection 缓冲已空则立即收
void WriteScheduler::request_close(const std::shared_ptr<Connection>& conn) {
    if (this->pending_writes_.find(conn) == this->pending_writes_.end()) {
        this->del_connection_(conn);
        return;
    }
    this->closing_.insert(conn);
}

void WriteScheduler::remove_pending(const std::shared_ptr<Connection>& conn) {
    this->pending_writes_.erase(conn);
    this->closing_.erase(conn);
}

// 高优先 TEXT 先于低优先 BINARY 排空
void WriteScheduler::drain_all() {
    this->drain_one(this->queue_high_);
    this->drain_one(this->queue_low_);
}

void WriteScheduler::drain_one(std::queue<PendingResponse>& q) {
    std::queue<PendingResponse> local{};
    std::swap(local, q);

    while (!local.empty()) {
        auto item = std::move(local.front());
        local.pop();

        int fd = item.conn_->sess_->fd_;
        // 连接已从循环拆除则丢弃 alive 在 io 关闭路径与摘除同步置 false
        if (!item.conn_->sess_->alive_) {
            continue;
        }

        // 该连接已有未发完数据 先追加保持帧顺序 再立刻尝试发送
        // 追加前判上限 慢客户端让缓冲一直涨，超限直接断开不再接收新帧
        if (this->pending_writes_.find(item.conn_) != this->pending_writes_.end()) {
            if (this->pending_bytes(item.conn_) > kMaxPendingBytes) {
                this->del_connection_(item.conn_);
                continue;
            }
            this->pending_writes_[item.conn_].append(std::move(item.data_));
            this->handle_write(item.conn_);
            continue;
        }

        std::string& wire = item.data_;
        auto [sent, failed] = this->try_send(fd, wire);

        if (failed) {
            // 硬错误直接收 未发段没有重试价值
            this->del_connection_(item.conn_);
            continue;
        }
        // 没发完 未发段进待写缓冲 注册写事件
        if (sent < static_cast<ssize_t>(wire.size())) {
            this->pending_writes_[item.conn_].append(wire.data() + sent, wire.size() - sent);
            // 写事件挂不上等于这条连接再也发不出去，直接收
            if (!this->loop_.mod_event(fd, EPOLLIN | EPOLLET | EPOLLOUT)) {
                this->del_connection_(item.conn_);
                continue;
            }
            // 立即尝试冲刷防 ET 饥饿
            this->handle_write(item.conn_);
            continue;
        }
        // 本批发完且缓冲已空 请求过冲刷后关闭的在此收
        if (this->closing_.count(item.conn_) > 0 &&
            this->pending_writes_.find(item.conn_) == this->pending_writes_.end()) {
            this->del_connection_(item.conn_);
        }
    }
}

// 该连接待写缓冲的字节数，无缓冲为 0
size_t WriteScheduler::pending_bytes(const std::shared_ptr<Connection>& conn) const {
    auto it = this->pending_writes_.find(conn);
    return it == this->pending_writes_.end() ? 0 : it->second.size();
}

// 发送循环 返回 已发送量 与 是否硬错误 遇 EAGAIN 中断不算硬错误
std::pair<ssize_t, bool> WriteScheduler::try_send(int fd, std::string_view view) {
    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(view.size())) {
        ssize_t n = send(fd, view.data() + sent, view.size() - sent, 0);
        if (n > 0) {
            sent += n;
        }
        else if (n < 0 && errno == EAGAIN) {
            break;
        }
        else {
            if (errno != EPIPE) {
                perror("send");
            }
            return {sent, true};
        }
    }
    return {sent, false};
}
