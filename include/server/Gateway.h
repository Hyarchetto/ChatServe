// Gateway — 主从服务器组装层 接收工厂产出的主/子 Reactor 组件 只做组装
// 主 Reactor 监听 accept 网关把 fd 按 fd 哈希分发到子 Reactor 处理
// 子 Reactor 各自独立事件循环线程 完全独立处理自己的一组连接
// 公共资源 ThreadPool/RoomManager 归应用层工厂持有 排池由 main 显式调用
// 生命周期与 Reactor 对齐 init/start_listen/loop/stop join 收在内部 loop 返回前收齐子线程
#pragma once

#include <memory>
#include <vector>
#include <thread>

#include "Reactor.h"

class Gateway {
public:
    // 接收工厂产出的组件 组装接线 子 Reactor 至少一个
    Gateway(std::unique_ptr<Reactor> main,
            std::vector<std::unique_ptr<Reactor>> subs);
    ~Gateway();

    // 初始化所有事件循环 任一失败返回 false
    bool init();

    // 主 Reactor 监听端口 失败返回 false
    bool start_listen(int port);

    // 拉起子 Reactor 线程并阻塞运行主 Reactor 直到 stop 返回前已收齐子线程
    void loop();

    // 停止所有事件循环
    void stop();

private:
    // 子 Reactor 与它的事件循环线程绑定为一个单元
    struct SubUnit {
        std::unique_ptr<Reactor> reactor;
        std::thread thread;
    };

    // 等待子 Reactor 线程退出
    void join();

    // 主 Reactor accept 到 fd 按 fd 哈希分发到子 Reactor
    void dispatch_fd(int fd);

    std::unique_ptr<Reactor> main_;               // 主 Reactor 只监听分发
    std::vector<SubUnit> subs_;                   // 子 Reactor + 各自事件循环线程
};
