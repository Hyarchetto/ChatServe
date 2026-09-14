// ChatServe 服务端入口
#include <iostream>
#include <csignal>
#include "../include/server/ReactorFactory.h"

#define kServerPort 8080

// io worker 数量
static constexpr size_t kIoCount = 4;

static Gateway* g_gateway = nullptr;
extern "C" void handle_signal(int) {
    if (g_gateway) {
        // 信号处理器只做异步信号安全操作，线程收尾交给 main 中 loop 返回后的收尾
        g_gateway->stop();
    }
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // 工厂持有中控共享资源 产主/io 组件并组装网关，主监听分发 io 处理连接，中控串行业务
    ReactorFactory factory;
    auto server = factory.create_gateway(kIoCount);
    g_gateway = server.get();

    if (!server->init()) {
        g_gateway = nullptr;
        std::cerr << "服务器初始化失败" << std::endl;
        return -1;
    }
    if (!server->start_listen(kServerPort)) {
        g_gateway = nullptr;
        std::cerr << "服务器监听失败" << std::endl;
        return -1;
    }

    // loop 阻塞运行，信号处理器调 stop 后收齐 io 线程并返回
    server->loop();

    // 事件循环已全部退出 信号不再需要服务服务器 清空指针避免悬垂
    g_gateway = nullptr;
    // Reactor 还存活 先停中控线程 避免线程访问已释放的对象
    factory.shutdown();
    std::cout << "服务器正常关闭" << std::endl;
    return 0;
}
