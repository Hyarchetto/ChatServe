// Acceptor — 监听端口，接受新连接
#include "server/Acceptor.h"
#include "core/EventLoop.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <iostream>

Acceptor::Acceptor(EventLoop& loop)
    : loop_(loop) {}

Acceptor::~Acceptor() {
    if (this->listenfd_ >= 0) {
        close(this->listenfd_);
    }
}

void Acceptor::start_listen(int port, NewConnectionFn on_new_connection) {
    this->on_new_connection_ = std::move(on_new_connection);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket error");
        return;
    }

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind error");
        close(listenfd);
        return;
    }
    if (listen(listenfd, 1024) < 0) {
        perror("listen error");
        close(listenfd);
        return;
    }

    int flags = fcntl(listenfd, F_GETFL, 0);
    fcntl(listenfd, F_SETFL, flags | O_NONBLOCK);

    this->listenfd_ = listenfd;

    this->loop_.add_event(listenfd,
                          EPOLLIN | EPOLLET,
                          [this, listenfd]() { this->accept_connections(listenfd); });

    std::cout << "服务器开始监听 " << port << std::endl;
    std::cout << "------------------------------------------" << std::endl;
}

void Acceptor::accept_connections(int listenfd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (true) {
        if (int clientfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_len);
                clientfd < 0) {

            if (errno == EAGAIN) {
                break;
            }
            else if (errno == EINTR) {
                continue;
            }
            else {
                perror("accept error");
                break;
            }
        }
        else {
            this->on_new_connection_(clientfd);
        }
    }
}
