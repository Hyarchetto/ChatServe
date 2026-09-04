// ReactorFactory — Reactor 形态与网关工厂 统一出服务器组件和组装好的网关
// 工厂生成什么返回什么 统一返回具体类型 Reactor/Gateway 不向上抽象
// 公共资源 ThreadPool/RoomManager 由工厂持有 生命周期归工厂
// 可选组件 Acceptor/ConnHandler 由 Reactor 内部创建 工厂按形态调用 create 触发
// 单 Reactor 完整可用 主从网关由 create_gateway 产组件并组装 网关不持工厂
#pragma once

#include <memory>

#include "Reactor.h"
#include "Gateway.h"
#include "../core/ThreadPool.h"
#include "../chatroom/Room.h"

class ReactorFactory {
public:
    // 生成单 Reactor 服务器 监听 accept 并自己处理连接 完整可用
    std::unique_ptr<Reactor> create_single();

    // 生成主 Reactor 监听 accept 后调用方 set_fd_handler 把 fd 交给网关
    std::unique_ptr<Reactor> create_main();

    // 生成从属 Reactor 不监听 网关把 fd 投递进来 处理一组连接
    std::unique_ptr<Reactor> create_sub();

    // 生成主从网关服务器 工厂产主/子 Reactor 组件并组装 主监听按 fd 哈希分发
    std::unique_ptr<Gateway> create_gateway(size_t sub_count = 4);

    // 排空线程池 必须在 Reactor 存活时调用 收尾在 loop 返回后执行
    void shutdown_pool() { this->works_.shutdown(); }

private:
    ThreadPool works_;        // 公共资源 生命周期归工厂
    RoomManager room_mgr_;    // 公共资源 生命周期归工厂
};
