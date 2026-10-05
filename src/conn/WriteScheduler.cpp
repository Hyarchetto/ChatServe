// 写引擎 — 本 loop 响应发送队列和部分发送管理
#include "conn/WriteScheduler.h"
#include "core/EventLoop.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

// 出站写出去了算一次活跃，但只对未升级的连接
// 已升级的连接由应用层起搏证明自己活着，往它上面写成功说明不了对端还在
static void stamp_outbound(const std::shared_ptr<Connection>& conn, ssize_t sent) {
    if (sent > 0 && !conn->ws_mode_) {
        conn->last_activity_ = std::chrono::steady_clock::now();
    }
}

WriteScheduler::WriteScheduler(EventLoop& loop, DelConnectionFn del_conn)
    : loop_(loop), del_connection_(std::move(del_conn)) {}

// 立刻把这一帧发给连接，没发完的部分转由 EPOLLOUT 推进
// 本函数不重入：它调出去的 handle_write 与 del_connection 一路到底都不回到这里
// 所以同连接的帧序就是调用序，中间不需要队列
void WriteScheduler::enqueue(const std::shared_ptr<Connection>& conn, std::string data) {
    // 连接已从循环拆除则丢弃 alive 在 io 关闭路径与摘除同步置 false
    if (!conn->sess_->alive_) {
        return;
    }

    // 该连接已有未发完数据，先追加保持帧顺序，再立刻尝试发送
    // 追加前判上限，慢客户端让缓冲一直涨，超限直接断开不再接收新帧
    if (this->pending_bytes(conn) != 0) {
        if (this->pending_bytes(conn) > kMaxPendingBytes) {
            this->del_connection_(conn);
            return;
        }
        this->pending_writes_[conn].append(std::move(data));
        this->handle_write(conn);
        return;
    }

    const int fd = conn->sess_->fd_;
    auto [sent, failed] = this->try_send(fd, data);
    if (failed) {
        // 硬错误直接收，未发段没有重试价值
        this->del_connection_(conn);
        return;
    }
    stamp_outbound(conn, sent);
    // 没发完，未发段进待写缓冲，注册写事件
    if (sent < static_cast<ssize_t>(data.size())) {
        this->pending_writes_[conn].append(std::move(data), static_cast<size_t>(sent));
        // 写事件挂不上等于这条连接再也发不出去，直接收
        if (!this->loop_.mod_event(fd, EPOLLIN | EPOLLET | EPOLLOUT)) {
            this->del_connection_(conn);
            return;
        }
        // 立即尝试冲刷防 ET 饥饿
        this->handle_write(conn);
        return;
    }
    // 缓冲已空，请求过冲刷后关闭的在此收
    if (this->closing_.find(conn) != this->closing_.end() && this->pending_bytes(conn) == 0) {
        this->del_connection_(conn);
    }
}

void WriteScheduler::handle_write(const std::shared_ptr<Connection>& conn) {
    int fd = conn->sess_->fd_;
    // 缓存已超上限，直接断开连接
    if (this->pending_bytes(conn) > kMaxPendingBytes) {
        this->del_connection_(conn);
        return;
    }
    auto it = this->pending_writes_.find(conn);
    // 不存在待发送数据，返回
    if (it == this->pending_writes_.end()) {
        return;
    }
    LazyBuffer& buf = it->second;
    auto [sent, failed] = this->try_send(fd, std::string_view(buf.data(), buf.size()));
    // 发送失败，断开连接
    if (failed) {
        this->del_connection_(conn);
        return;
    }
    stamp_outbound(conn, sent);
    // 完成发送
    if (sent >= static_cast<ssize_t>(buf.size())) {
        this->pending_writes_.erase(conn);
        // 连接还存活才删除写事件
        if (conn->sess_->alive_) {
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET);
        }
        // 缓冲发空，请求过冲刷后关闭的在此收
        if (this->closing_.find(conn) != this->closing_.end()) {
            this->del_connection_(conn);
        }
    }
    else {
        buf.consume(sent);
    }
}

// 数据未完全发送 — 待写数据发完再回调 del_connection，缓冲已空则立即收
void WriteScheduler::request_close(const std::shared_ptr<Connection>& conn) {
    if (this->pending_bytes(conn) == 0) {
        this->del_connection_(conn);
        return;
    }
    this->closing_.insert(conn);
}

void WriteScheduler::remove_pending(const std::shared_ptr<Connection>& conn) {
    this->pending_writes_.erase(conn);
    this->closing_.erase(conn);
}

// 该连接待写缓冲的字节数，无缓冲为 0
size_t WriteScheduler::pending_bytes(const std::shared_ptr<Connection>& conn) const {
    auto it = this->pending_writes_.find(conn);
    return it == this->pending_writes_.end() ? 0 : it->second.size();
}

// 发送循环，返回已发送量与是否硬错误，遇 EAGAIN 中断不算硬错误
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
