// Gateway — 主从服务器组装层 接收工厂产出的主/子 Reactor 组件 只做组装
// 主 Reactor 监听 accept 网关把 fd 按 fd 哈希分发到 io 从属 Reactor 线程
// 从属 Reactor 是 io worker 连接泵 业务全在中控线程 中控归工厂持有与启停
// 生命周期 init/start_listen/loop/stop join 收在内部 loop 返回前收齐 io 线程
#pragma once

#include <memory>
#include <vector>
#include <thread>

#include "Reactor.h"

class Gateway {
public:
    // 接收工厂产出的组件 组装接线 从 Reactor 至少一个
    Gateway(std::unique_ptr<Reactor> main,
            std::vector<std::unique_ptr<Reactor>> subs);
    ~Gateway();

    // 初始化所有事件循环 任一失败返回 false
    bool init();

    // 主 Reactor 监听端口 失败返回 false
    bool start_listen(int port);

    // 拉起 io Reactor 线程并阻塞运行主 Reactor 直到 stop 返回前已收齐 io 线程
    void loop();

    // 停止所有事件循环
    void stop();

private:
    // io Reactor 与它的事件循环线程绑定为一个单元
    struct SubUnit {
        std::unique_ptr<Reactor> reactor;
        std::thread thread;
    };

    // 等待 io Reactor 线程退出
    void join();

    // 主 Reactor accept 到 fd 按 fd 哈希分发到 io Reactor
    void dispatch_fd(int fd);

    std::unique_ptr<Reactor> main_;               // 主 Reactor 只监听分发
    std::vector<SubUnit> subs_;                   // io Reactor + 各自事件循环线程
};
