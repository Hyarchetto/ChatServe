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
    if (this->listen_fd_ >= 0) {
        close(this->listen_fd_);
    }
}

int Acceptor::start_listen(int port) {
    // 已监听则直接返回，防止重复调用导致泄漏
    if (this->listen_fd_ >= 0) {
        return this->listen_fd_;
    }
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket error");
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind error");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 1024) < 0) {
        perror("listen error");
        close(listen_fd);
        return -1;
    }

    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    this->listen_fd_ = listen_fd;

    std::cout << "服务器开始监听 " << port << std::endl;
    std::cout << "------------------------------------------" << std::endl;

    return listen_fd;
}

void Acceptor::accept_connections(int listen_fd, NewConnectionFn on_new_connection) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (true) {
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
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
            on_new_connection(client_fd);
        }
    }
}
