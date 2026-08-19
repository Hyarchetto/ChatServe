// 单 Reactor 多线程 TCP 服务器

#include "server/Reactor.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

// ======================================== 构造析构 ========================================
Reactor::Reactor()
    : writer_(this->loop_, [this](const auto& c) {
          this->close_connection(c);
      })
    , acceptor_(this->loop_)
    , http_handler_(this->loop_, this->works_, this->http_router_, this->writer_,
                    [this](const auto& c) { this->close_connection(c); })
    , ws_handler_(this->loop_, this->works_, this->ws_app_router_,
                  this->room_mgr_, this->transfer_mgr_,
                  this->conn_registry_, this->writer_) {}

Reactor::~Reactor() {
    this->stop();
}

bool Reactor::init() {
    return this->loop_.init();
}

void Reactor::stop() {
    this->loop_.quit();
    this->works_.shutdown();
}

// 信号处理器专用 只做异步信号安全操作 不做线程池收尾
void Reactor::request_stop() {
    this->loop_.quit();
}

// ======================================== 监听 委托给 Acceptor ========================================

void Reactor::start_listen(int port) {
    this->acceptor_.start_listen(port,
        [this](int fd) { this->add_connection(fd); });
}

// ======================================== 事件循环 ========================================

void Reactor::loop() {
    this->loop_.loop();
}

// ======================================== 连接管理 ========================================
void Reactor::add_connection(int fd) {
    auto conn = std::make_shared<Connection>(fd);
    this->conn_registry_.add(fd, conn);
    this->loop_.add_event(fd, EPOLLIN | EPOLLET,
        [this, conn]() { this->handle_clientfd(conn); },
        [this, conn]() { this->writer_.handle_write(conn); },
        [this, conn]() { this->close_connection(conn); });
}
// ======================================== 关闭连接 ========================================
void Reactor::close_connection(const std::shared_ptr<Connection>& conn) {
    // 通用销毁
    this->del_connection(conn);
    // 协议清理
    if (conn->ws_mode_) {
        this->ws_handler_.cleanup(conn);
    }
    // HTTP 无跨连接状态，无需额外清理
}
// ======================================== 通用关闭 ========================================
void Reactor::del_connection(const std::shared_ptr<Connection>& conn) {
    int fd = conn->fd_;
    conn->alive_ = false;
    this->conn_registry_.remove(fd);
    this->writer_.remove_pending(conn);
    this->loop_.del_event(fd);
}

// ======================================== 读取数据 ========================================

bool Reactor::read_data(const std::shared_ptr<Connection>& conn) {
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

void Reactor::handle_clientfd(const std::shared_ptr<Connection>& conn) {
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