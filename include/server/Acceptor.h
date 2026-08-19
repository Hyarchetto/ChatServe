// Acceptor — 监听端口，接受新连接
#pragma once

#include <functional>

#include "../core/EventLoop.h"

class Acceptor {
public:
    using NewConnectionFn = std::function<void(int fd)>;

    Acceptor(EventLoop& loop);
    ~Acceptor();

    void start_listen(int port, NewConnectionFn on_new_connection);

private:
    void accept_connections(int listenfd);

    EventLoop& loop_;
    int listenfd_ = -1;
    NewConnectionFn on_new_connection_;
};
