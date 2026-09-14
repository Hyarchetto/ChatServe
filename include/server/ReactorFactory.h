// ReactorFactory — Reactor 形态与服务器工厂
// 工厂生成什么返回什么 统一返回具体类型 Reactor/Gateway 不向上抽象
// 公共资源 业务线程池 ThreadPool 与 中控 CtrlDispatcher 归工厂持有 生命周期归工厂
// 命令经中控 submit 进业务线程池执行
// 可选组件 Acceptor/ConnHandler 由 Reactor 内部创建 工厂按形态调用 create 触发
// 单 Reactor 完整可用 主从工作者网关由 create_gateway 产组件并组装 网关不持工厂
#pragma once

#include <cstddef>
#include <memory>

#include "../core/ThreadPool.h"
#include "CtrlDispatcher.h"
#include "Gateway.h"
#include "Reactor.h"

class ReactorFactory {
public:
    // 构造即建业务线程池与中控 中控线程在装配完成后启动
    ReactorFactory();
    // 停中控与业务线程池 供单独部署的调用方收尾
    ~ReactorFactory();

    // 生成单 Reactor 服务器 监听 accept 并自己处理连接 完整可用
    std::unique_ptr<Reactor> create_single();

    // 生成主 Reactor 监听 accept 后调用方 set_fd_handler 把 fd 交给网关
    std::unique_ptr<Reactor> create_main();

    // 生成 io 从属 Reactor 不监听 网关把 fd 投递进来 本线程泵连接 每调用分配 io 序号
    std::unique_ptr<Reactor> create_sub();

    // 生成主从工作者服务器 工厂产主/io 组件并组装 主监听按 fd 哈希分发
    std::unique_ptr<Gateway> create_gateway(size_t sub_count = 4);

    // 停中控线程并排空业务线程池 服务器 loop 返回后显式调用 幂等
    void shutdown();

private:
    // 装配完成后启动中控线程 幂等
    void start_dispatcher();

    std::unique_ptr<ThreadPool> works_;              // 业务线程池 公共资源 归工厂
    std::unique_ptr<CtrlDispatcher> dispatcher_;     // 中控 公共资源 归工厂
    size_t next_io_ = 0;                             // 已分配的 io 序号
};
