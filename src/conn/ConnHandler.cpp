// ConnHandler — 连接生命周期管理器
#include "conn/ConnHandler.h"
#include "conn/Connection.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

// ======================================== 构造 ========================================
ConnHandler::ConnHandler(EventLoop& loop, ThreadPool& works,
                         RoomManager& room_mgr)
    : loop_(loop)
    , writer_(loop, [this](const auto& c) { this->close_connection(c); })
    , http_handler_(loop, works)
    , ws_handler_(loop, works, room_mgr) {}

// ======================================== 连接管理 ========================================
void ConnHandler::add_connection(int fd) {
    // 连接所有权由事件循环回调捕获的 shared_ptr 持有 不再维护单独连接表
    // 构造即绑定归属写引擎 发送投递以它为目标
    auto conn = std::make_shared<Connection>(fd, this->writer_);
    this->loop_.add_event(fd, EPOLLIN | EPOLLET,
        [this, conn]() { this->handle_clientfd(conn); },
        [this, conn]() { this->writer_.handle_write(conn); },
        [this, conn]() { this->close_connection(conn); });
}

// ======================================== 关闭连接 ========================================
void ConnHandler::close_connection(const std::shared_ptr<Connection>& conn) {
    // 通用关闭
    this->del_connection(conn);
    // 协议清理
    if (conn->ws_mode_) {
        this->ws_handler_.cleanup(conn);
    }
    // HTTP 无跨连接状态，无需额外清理
}

// ======================================== 通用关闭 ========================================
void ConnHandler::del_connection(const std::shared_ptr<Connection>& conn) {
    conn->alive_ = false;
    this->writer_.remove_pending(conn);
    this->loop_.del_event(conn->fd_);
}

// ======================================== 读取数据 ========================================

bool ConnHandler::read_data(const std::shared_ptr<Connection>& conn) {
    int clientfd = conn->fd_;
    char temp_buffer[BUFFER_SIZE];

    while (true) {
        ssize_t bytes_recv = recv(clientfd, temp_buffer, sizeof(temp_buffer), 0);
        if (bytes_recv > 0) {
            conn->read_buf_.append(temp_buffer, bytes_recv);
            continue;
        }
        if (bytes_recv == 0) {
            this->close_connection(conn);
            return false;
        }
        else{
            if (errno == EAGAIN) {
                break;
            }
            else{
                perror("recv");
                this->close_connection(conn);
                return false;
            }
        }
    }
    return true;
}

// ======================================== 客户端数据总入口 ========================================

void ConnHandler::handle_clientfd(const std::shared_ptr<Connection>& conn) {
    // 啥也没读到，直接返回
    if (!this->read_data(conn) || conn->read_buf_.empty()) {
        return;
    }
    // 根据连接协议选择不同的处理方式
    else{
        if (conn->ws_mode_) {
            this->ws_handler_.handle_ws(conn);
        }
        else {
            this->http_handler_.handle_http(conn);
        }
    }
}
