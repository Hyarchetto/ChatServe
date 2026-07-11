// 连接接收器的实现
// Acceptor 封装了 TCP 服务器的标准流程：
// socket 创建、地址绑定、端口监听、非阻塞 accept
#include "server/Acceptor.h"
#include "core/EventLoop.h"

#include <fcntl.h>
#include <cstdio>

Acceptor::Acceptor() {}

// 析构时关闭监听套接字
Acceptor::~Acceptor() {
    if (this->listenfd_ >= 0) {
        close(this->listenfd_);
    }
}

// 启动 TCP 监听服务
//
// 执行步骤如下：
//   1. 创建 TCP socket
//   2. 设置 SO_REUSEADDR 选项，允许端口重用
//   3. 绑定到指定端口
//   4. 开始监听，backlog 设为 1024
//   5. 设置 socket 为非阻塞模式
//   6. 把监听套接字注册到 EventLoop 中，
//      采用边缘触发模式监听可读事件
//
// 边缘触发模式配合 while 循环可以一次性收尽所有新连接，
// 避免多次触发 epoll 事件
//
// 返回 true 表示启动成功
bool Acceptor::start(int port, EventLoop* loop) {
    this->loop_ = loop;

    this->listenfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (this->listenfd_ < 0) {
        perror("socket error");
        return false;
    }

    int opt = 1;
    setsockopt(this->listenfd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(this->listenfd_, (struct sockaddr*)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind error");
        close(this->listenfd_);
        this->listenfd_ = -1;
        return false;
    }
    if (listen(this->listenfd_, 1024) < 0) {
        perror("listen error");
        close(this->listenfd_);
        this->listenfd_ = -1;
        return false;
    }

    int flags = fcntl(this->listenfd_, F_GETFL, 0);
    fcntl(this->listenfd_, F_SETFL, flags | O_NONBLOCK);

    this->loop_->add_event(this->listenfd_, EPOLLIN | EPOLLET,
        [this]() { this->handle_accept(); });

    return true;
}

// 设置新连接到达时的回调函数
// 这个回调通常由 MainReactor 提供，用于把新连接分发给子 Reactor
void Acceptor::set_new_connection_callback(NewConnectionCallback cb) {
    this->new_conn_cb_ = std::move(cb);
}

// 处理 accept 事件
//
// 由于采用边缘触发模式，使用 while 循环一次性收尽所有新连接
// 每次 accept 后立即调用 new_conn_cb_ 回调，
// 把连接交给上层处理
//
// 如果 accept 返回 EAGAIN 表示已经没有新连接了，退出循环
// 如果 accept 返回 EINTR 表示被信号中断，继续尝试
// 其他错误直接退出循环以免死循环
void Acceptor::handle_accept() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (true) {
        int clientfd = accept(this->listenfd_,
                              (struct sockaddr*)&client_addr,
                              &client_len);
        if (clientfd < 0) {
            if (errno == EAGAIN) break;
            if (errno == EINTR) continue;
            perror("accept error");
            break;
        }
        if (this->new_conn_cb_) {
            this->new_conn_cb_(clientfd);
        } else {
            close(clientfd);
        }
    }
}
