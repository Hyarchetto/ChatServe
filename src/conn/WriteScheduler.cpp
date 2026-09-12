// 写引擎 — 本 loop 响应发送队列和部分发送管理
#include "conn/WriteScheduler.h"
#include "core/EventLoop.h"

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

// 高优先 TEXT 先于低优先 BINARY 排空
void WriteScheduler::drain_all() {
    this->drain_one(this->queue_high_);
    this->drain_one(this->queue_low_);
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

        // 该连接已有未发完数据 直接追加保持帧顺序 再立刻尝试发送
        if (auto p_it = this->pending_writes_.find(item.conn_);
            p_it != this->pending_writes_.end()) {
            p_it->second.append(std::move(item.data_));
            this->handle_write(item.conn_);
            continue;
        }

        std::string& wire = item.data_;
        auto [sent, failed] = this->try_send(fd, wire);

        if (failed) {
            // 出错 标记关闭 收尾交给下方统一判断
            item.conn_->sess_->close_ = true;
        }
        // 没发完 未发段进待写缓冲 注册写事件
        else if (sent < static_cast<ssize_t>(wire.size())) {
            this->pending_writes_[item.conn_].append(
                wire.data() + sent, wire.size() - sent);
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET | EPOLLOUT);
            // 立即尝试冲刷防 ET 饥饿
            this->handle_write(item.conn_);
            continue;
        }
        // 发完或出错 存在关闭信号则断开
        if (item.conn_->sess_->close_) {
            this->del_connection_(item.conn_);
        }
    }
}

void WriteScheduler::handle_write(const std::shared_ptr<Connection>& conn) {
    int fd = conn->sess_->fd_;
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
        // 发完新消息入队即排空 此处无待发消息无需再冲刷
        if (conn->sess_->close_) {
            this->del_connection_(conn);
        }
    }
    else {
        buf.consume(sent);
    }
}

void WriteScheduler::remove_pending(const std::shared_ptr<Connection>& conn) {
    this->pending_writes_.erase(conn);
}
