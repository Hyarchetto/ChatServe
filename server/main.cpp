// ChatServe 服务端入口
// 信号注册在最上层应用层，与核心逻辑解耦
#include <iostream>
#include <csignal>
#include "../include/server/ReactorFactory.h"

#define SERVER_PORT 8080

// 子 Reactor 数量 每个独立事件循环线程 网关按 fd 哈希分发
static constexpr size_t SUB_COUNT = 4;

static Gateway* g_gateway = nullptr;

extern "C" void handle_signal(int) {
    if (g_gateway) {
        // 信号处理器只做异步信号安全操作 线程池收尾交给 main 中 loop 返回后的收尾
        g_gateway->stop();
    }
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // 工厂持有共享资源 ThreadPool/RoomManager 归属应用层 生产组件与网关
    ReactorFactory factory;

    // 工厂组装主从网关 主监听按 fd 哈希分发到子 Reactor 网关不持工厂
    auto server = factory.create_gateway(SUB_COUNT);
    g_gateway = server.get();

    if (!server->init()) {
        g_gateway = nullptr;
        std::cerr << "服务器初始化失败" << std::endl;
        return -1;
    }
    if (!server->start_listen(SERVER_PORT)) {
        g_gateway = nullptr;
        std::cerr << "服务器监听失败" << std::endl;
        return -1;
    }

    // loop 阻塞运行 信号处理器调 stop 后返回 返回前已收齐子线程
    server->loop();

    // 事件循环已全部退出 信号不再需要服务服务器 清空指针避免悬垂
    g_gateway = nullptr;
    // Reactor 还存活 先排空线程池 避免任务访问已释放的协议处理器
    factory.shutdown_pool();
    std::cout << "服务器正常关闭" << std::endl;
    return 0;
}
