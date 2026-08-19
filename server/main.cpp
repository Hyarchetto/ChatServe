// ChatServe 服务端入口
// 信号注册在最上层应用层，与核心逻辑解耦
#include <iostream>
#include <csignal>
#include "../include/server/Reactor.h"

#define SERVER_PORT 8080

static Reactor* g_reactor = nullptr;

extern "C" void handle_signal(int) {
    if (g_reactor) {
        // 信号处理器只做异步信号安全操作 线程池收尾交给 main 中 loop 返回后的 stop
        g_reactor->request_stop();
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

    // loop 已返回 信号处理器只唤醒事件循环 在此安全收尾线程池
    server.stop();
    // 事件循环已退出 信号不再需要服务 reactor 清空指针避免悬垂
    g_reactor = nullptr;
    std::cout << "服务器正常关闭" << std::endl;
    return 0;
}
