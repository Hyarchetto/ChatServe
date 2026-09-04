// Acceptor — 监听端口接受新连接 纯监听逻辑 不接触事件循环
#include "server/Acceptor.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <iostream>

Acceptor::Acceptor() = default;

Acceptor::~Acceptor() {
    if (this->listenfd_ >= 0) {
        close(this->listenfd_);
    }
}

int Acceptor::start_listen(int port) {
    // 已监听则直接返回，防止重复调用导致泄漏
    if (this->listenfd_ >= 0) {
        return this->listenfd_;
    }
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket error");
        return -1;
    }

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenfd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind error");
        close(listenfd);
        return -1;
    }
    if (listen(listenfd, 1024) < 0) {
        perror("listen error");
        close(listenfd);
        return -1;
    }

    int flags = fcntl(listenfd, F_GETFL, 0);
    fcntl(listenfd, F_SETFL, flags | O_NONBLOCK);

    this->listenfd_ = listenfd;

    std::cout << "服务器开始监听 " << port << std::endl;
    std::cout << "------------------------------------------" << std::endl;

    return listenfd;
}

void Acceptor::accept_connections(int listenfd, NewConnectionFn on_new_connection) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (true) {
        int clientfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_len);
        if (clientfd < 0) {
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
            on_new_connection(clientfd);
        }
    }
}
