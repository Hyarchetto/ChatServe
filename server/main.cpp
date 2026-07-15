// ChatServe 服务端入口
// 信号注册在最上层应用层，与核心逻辑解耦
#include <iostream>
#include <csignal>
#include "../include/server/Reactor.h"

#define SERVER_PORT 8080

static Reactor* g_reactor = nullptr;

extern "C" void handle_signal(int) {
    if (g_reactor) {
        g_reactor->stop();
    }
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    Reactor server;
    g_reactor = &server;
    if (!server.init()) {
        g_reactor = nullptr;
        std::cerr << "服务器初始化失败" << std::endl;
        return -1;
    }
    server.start_listen(SERVER_PORT);
    server.loop();

    std::cout << "服务器正常关闭" << std::endl;
    return 0;
}
