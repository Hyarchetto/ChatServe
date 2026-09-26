// Reactor — 统一事件循环服务器单元
#include "server/Reactor.h"
#include "server/Acceptor.h"

#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>

Reactor::Reactor() {
    // fd 去路兜底 未绑定前接收的 fd 打日志并关闭 避免空函数调用崩溃和 fd 泄漏
    this->fd_handler_ = [](int fd) {
        std::cerr << "fd 去路未绑定 丢弃 fd=" << fd << std::endl;
        close(fd);
    };
}

Reactor::~Reactor() {
    this->stop();
}

// 创建监听组件 Acceptor
void Reactor::create_acceptor() {
    this->acceptor_ = std::make_unique<Acceptor>();
}

// 创建连接处理器并绑定 fd 去路 io worker 直接把 fd 交给本地 ConnHandler
void Reactor::create_handler(int io_index, Mailbox<CtrlUp>& ctrl_uplink_box) {
    this->conn_handler_ = std::make_unique<ConnHandler>(this->loop_, io_index, ctrl_uplink_box);
    this->fd_handler_ = [this](int fd) {
        this->conn_handler_->add_connection(fd);
    };
}

void Reactor::set_fd_handler(std::function<void(int)> handler) {
    this->fd_handler_ = std::move(handler);
}

// 创建 epoll 与 eventfd 从属形态顺带把连接处理器的 fd 挂上
// 挂载要往事件表里加 fd 必须在 init 之后 loop 之前 本函数正在这个位置
bool Reactor::init() {
    if (!this->loop_.init()) {
        return false;
    }
    // 主形态只有监听没有连接表 无 fd 可挂
    if (!this->conn_handler_) {
        return true;
    }
    return this->conn_handler_->attach();
}

// 内部 Acceptor 只提供监听逻辑 事件循环不暴露 由本类把监听 fd 挂入内部 epoll
bool Reactor::start_listen(int port) {
    if (!this->acceptor_) {
        std::cerr << "Acceptor未创建" << std::endl;
        return false;
    }
    int listen_fd = this->acceptor_->start_listen(port);
    if (listen_fd < 0) {
        return false;
    }
    // 挂不上监听这个端口就永远收不到连接，让启动直接失败
    return this->loop_.add_event(listen_fd, EPOLLIN | EPOLLET,
        [this, listen_fd]() {
            this->acceptor_->accept_connections(listen_fd, [this](int fd) { this->fd_handler_(fd); });
        });
}

// 从属入口 网关从其他线程投递 fd 通过 post 切到本事件循环执行
// 直接跨线程调 add_connection 会改 event_map_ 非线程安全
void Reactor::add_connection(int fd) {
    this->loop_.post([this, fd]() { this->fd_handler_(fd); });
}

Mailbox<CtrlDown>& Reactor::downlink_box() {
    return this->conn_handler_->downlink_box();
}

void Reactor::loop() {
    this->loop_.loop();
}

void Reactor::stop() {
    this->loop_.quit();
}
