// 写调度器 — 响应发送队列和部分发送管理
#include "conn/WriteScheduler.h"
#include "core/EventLoop.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

WriteScheduler::WriteScheduler(EventLoop& loop, DelConnectionFn del_conn)
    : loop_(loop), del_connection_(std::move(del_conn)) {}

void WriteScheduler::push_response(const std::shared_ptr<Connection>& conn,
                                   std::string data, bool is_high_priority) {
    if (is_high_priority) {
        this->queue_high_.emplace(PendingResponse{conn, std::move(data)});
    } 
    else {
        this->queue_low_.emplace(PendingResponse{conn, std::move(data)});
    }
}

void WriteScheduler::flush_responses() {
    // 防止递归调用死锁
    if (this->flushing_) {
        return;
    }
    else{
        this->flushing_ = true;
        this->drain_one(this->queue_high_);
        this->drain_one(this->queue_low_);
        this->flushing_ = false;
    }
}

void WriteScheduler::drain_one(std::queue<PendingResponse>& q) {
    std::queue<PendingResponse> local{};
    std::swap(local, q);

    while (!local.empty()) {
        auto item = std::move(local.front());
        local.pop();

        int fd = item.conn_->fd_;
        // 连接已从循环拆除则丢弃 alive_ 在 del_connection 与摘除同步置 false
        if (!item.conn_->alive_) {
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
        ssize_t total = static_cast<ssize_t>(wire.size());
        ssize_t sent = 0;

        while (sent < total) {
            if (ssize_t n = send(fd, wire.data() + sent, total - sent, 0);
                        n > 0) {
                sent += n;
            }
            else if (n == 0) {
                perror("send: 合法失败");
                break;
            }
            else {
                if (errno == EAGAIN) {
                    break;
                }
                else if (errno == EPIPE) {
                    item.conn_->pending_close_ = true;
                    sent = total;
                }
                else {
                    perror("send");
                    item.conn_->pending_close_ = true;
                    sent = total;
                }
            }
        }

        if (sent >= total) {
            // 发送完且存在关闭信号则断开连接
            if (item.conn_->pending_close_) {
                this->del_connection_(item.conn_);
            }
        }
        // 没发完的连接注册写事件 未发段直接追加进待写缓冲
        else {
            this->pending_writes_[item.conn_].append(wire.data() + sent, wire.size() - sent);
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET | EPOLLOUT);
            // 立即尝试冲刷，防止ET饥饿
            this->handle_write(item.conn_);
        }
    }
}

void WriteScheduler::handle_write(const std::shared_ptr<Connection>& conn) {
    int fd = conn->fd_;

    auto it = this->pending_writes_.find(conn);
    if (it == this->pending_writes_.end()) {
        return;
    }
    LazyBuffer& buf = it->second;
    ssize_t total = static_cast<ssize_t>(buf.size());
    ssize_t sent = 0;

    while (sent < total) {
        if (ssize_t n = send(fd, buf.data() + sent, total - sent, 0);
                    n > 0) {
            sent += n;
        }
        else if (n == 0) {
            perror("send: 合法失败");
            break;
        }
        else {
            if (errno == EAGAIN) {
                break;
            }
            else if (errno == EPIPE) {
                this->pending_writes_.erase(conn);
                this->del_connection_(conn);
                return;
            }
            else {
                perror("send");
                this->pending_writes_.erase(conn);
                this->del_connection_(conn);
                return;
            }
        }
    }

    if (sent >= total) {
        this->pending_writes_.erase(conn);
        // 连接还存活才删除写事件
        if (conn->alive_) {
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET);
        }
        // 连接可写了，处理之前因 EAGAIN 被重入队列的消息
        this->flush_responses();
        if (conn->pending_close_) {
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
