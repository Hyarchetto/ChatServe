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
void Reactor::create_handler(int io_index, Mailbox<CtrlUp>& ctrl_inbox) {
    this->conn_handler_ = std::make_unique<ConnHandler>(this->loop_, io_index,
                                                        ctrl_inbox);
    this->fd_handler_ = [this](int fd) {
        this->conn_handler_->add_connection(fd);
    };
}

Mailbox<CtrlDown>& Reactor::outbox() {
    return this->conn_handler_->outbox();
}

void Reactor::set_fd_handler(std::function<void(int)> handler) {
    this->fd_handler_ = std::move(handler);
}

bool Reactor::init() {
    return this->loop_.init();
}

// 内部 Acceptor 只提供监听逻辑 事件循环不暴露 由本类把监听 fd 挂入内部 epoll
bool Reactor::start_listen(int port) {
    if (!this->acceptor_) {
        std::cerr << "Acceptor未创建" << std::endl;
        return false;
    }
    int listenfd = this->acceptor_->start_listen(port);
    if (listenfd < 0) {
        return false;
    }
    this->loop_.add_event(listenfd, EPOLLIN | EPOLLET,
        [this, listenfd]() {
            this->acceptor_->accept_connections(listenfd,
                [this](int fd) { this->fd_handler_(fd); });
        });
    return true;
}

// 从属入口 网关从其他线程投递 fd 通过 post 切到本事件循环执行
// 直接跨线程调 add_connection 会改 event_map_ 非线程安全
void Reactor::add_connection(int fd) {
    this->loop_.post([this, fd]() { this->fd_handler_(fd); });
}

void Reactor::loop() {
    this->loop_.loop();
}

void Reactor::stop() {
    this->loop_.quit();
}
