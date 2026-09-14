// Acceptor — 监听端口接受新连接 纯监听逻辑 不接触事件循环
// 事件循环由外部持有 将监听 fd 挂入 epoll 触发后调用 accept_connections
#pragma once

#include <functional>

class Acceptor {
public:
    using NewConnectionFn = std::function<void(int fd)>;

    Acceptor();
    ~Acceptor();

    // 创建 socket、bind、listen 返回监听 fd 失败返回 -1
    int start_listen(int port);

    // accept 监听 fd 上所有就绪连接 逐个回调 由事件循环读回调调用
    void accept_connections(int listen_fd, NewConnectionFn on_new_connection);

private:
    int listen_fd_ = -1;
};
