// ChatServe 服务端入口
// 信号注册在最上层应用层，与核心逻辑解耦
#include <iostream>
#include <csignal>
#include "../include/server/ReactorFactory.h"

#define SERVER_PORT 8080

static Reactor* g_reactor = nullptr;

extern "C" void handle_signal(int) {
    if (g_reactor) {
        // 信号处理器只做异步信号安全操作 线程池收尾交给 main 中 loop 返回后的收尾
        g_reactor->stop();
    }
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // 工厂持有公共资源 ThreadPool/RoomManager 生命周期归工厂
    // 非全局资源 Acceptor 由工厂生成注入 Reactor 不再在此创建
    ReactorFactory factory;

    // 工厂生成服务器 单 Reactor 与主从模型一行切换
    auto server = factory.create_single();
    g_reactor = server.get();

    if (!server->init()) {
        g_reactor = nullptr;
        std::cerr << "服务器初始化失败" << std::endl;
        return -1;
    }
    server->start_listen(SERVER_PORT);
    server->loop();

    // loop 已返回 信号处理器只唤醒事件循环 在此安全收尾线程池
    server->stop();
    // 事件循环已退出 信号不再需要服务服务器 清空指针避免悬垂
    g_reactor = nullptr;
    // Reactor 还存活 先排空线程池 避免任务访问已释放的协议处理器
    factory.shutdown_pool();
    std::cout << "服务器正常关闭" << std::endl;
    return 0;
}
