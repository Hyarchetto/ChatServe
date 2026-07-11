// ChatServe 服务端入口
// 主从多 Reactor 架构模型
// ./build.sh
// ./build/server/chat_server

#include <iostream>
#include <csignal>
#include "../include/server/MainReactor.h"

// 服务器监听端口
#define SERVER_PORT 8080

// 子 Reactor 的数量
#define SUB_REACTOR_COUNT 4

int main() {
    MainReactor server(SERVER_PORT, SUB_REACTOR_COUNT);
    if (!server.init()) {
        std::cerr << "服务器初始化失败" << std::endl;
        return -1;
    }
    server.start();
    return 0;
}
