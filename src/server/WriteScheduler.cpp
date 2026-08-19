// 写调度器 — 响应发送队列和部分发送管理
#include "server/WriteScheduler.h"
#include "core/EventLoop.h"

#include <unordered_set>
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
    if (this->flushing_) return;
    this->flushing_ = true;
    drain_one(this->queue_high_);
    drain_one(this->queue_low_);
    this->flushing_ = false;
}

void WriteScheduler::drain_one(std::queue<PendingResponse>& q) {
    std::queue<PendingResponse> local{};
    std::swap(local, q);

    // 跟踪因 EAGAIN 未发完的 fd，跳过后续同 fd 消息避免帧交织
    std::unordered_set<int> pending_fds;

    while (!local.empty()) {
        auto item = std::move(local.front());
        local.pop();

        int fd = item.conn_->fd_;
        if (!this->loop_.has_event(fd)) {
            continue;
        }

        // 该 fd 上还有未发完数据，将消息放回队列等 EPOLLOUT 恢复后再发
        if (pending_fds.count(fd)) {
            q.push(std::move(item));
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
                    pending_fds.insert(fd);
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
        // 没发完的连接注册写事件
        else {
            std::string remainder = wire.substr(sent);
            auto [it, inserted] = this->pending_writes_.try_emplace(item.conn_, std::move(remainder));
            if (!inserted) {
                it->second.append(remainder);
            }
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET | EPOLLOUT);
            // 立即尝试冲刷，防止ET饥饿
            this->handle_write(item.conn_);
        }
    }
}

void WriteScheduler::handle_write(const std::shared_ptr<Connection>& conn) {
    int fd = conn->fd_;

    auto it = this->pending_writes_.find(conn);
    if (it == this->pending_writes_.end()) return;

    std::string& data = it->second;
    ssize_t total = static_cast<ssize_t>(data.size());
    ssize_t sent = 0;

    while (sent < total) {
        if (ssize_t n = send(fd, data.data() + sent, total - sent, 0);
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
        // 还有效则删除写事件
        if (this->loop_.has_event(fd)) {
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET);
        }
        // 连接可写了，处理之前因 EAGAIN 被重入队列的消息
        this->flush_responses();
        if (conn->pending_close_) {
            this->del_connection_(conn);
        }
    }
    else {
        data.erase(0, sent);
    }
}

void WriteScheduler::remove_pending(const std::shared_ptr<Connection>& conn) {
    this->pending_writes_.erase(conn);
}
