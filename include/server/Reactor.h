// Reactor — 统一事件循环服务器单元 一个类三种形态 工厂注入组件回调生成
// 单 Reactor   监听 accept 并自己处理连接
// 主 Reactor   监听 accept 把 fd 交给网关回调 由网关分发到从属
// 从属 Reactor 不监听 网关把 fd 投递进来 自己处理一组连接
// Reactor 是组成服务器的单元 不继承服务器抽象 完整服务器由网关组装
// 公共资源 ThreadPool/RoomManager 由工厂持有 处理连接时经 create_handler 注入
// 可选组件 Acceptor/ConnHandler 由本类内部创建 工厂按形态调用 create 触发
#pragma once

#include <functional>
#include <memory>

#include "Acceptor.h"
#include "../conn/ConnHandler.h"
#include "../core/EventLoop.h"
#include "../core/ThreadPool.h"
#include "../chatroom/Room.h"

class Reactor {
public:
    // 构造只建事件循环并给 fd 去路兜底 组件由工厂按形态 create 注入
    Reactor();
    ~Reactor();

    // 创建监听组件 Acceptor 单和主形态由工厂调用
    void create_acceptor();
    // 创建连接处理器并绑定 fd 去路 公共资源经此注入 单和从属形态由工厂调用
    void create_handler(ThreadPool& works, RoomManager& room_mgr);
    // 修改 fd 去路
    void set_fd_handler(std::function<void(int)> handler);
    // 创建 epoll 和 eventfd
    bool init();

    // 内部 Acceptor 监听端口 accept 到的 fd 走 fd_handler_
    void start_listen(int port);

    // 从属入口 网关投递新连接 fd 内部切到本事件循环执行
    void add_connection(int fd);

    // 事件循环主函数 阻塞直到服务器退出
    void loop();

    // 停止事件循环 quit 通过原子标志加eventfd唤醒
    void stop();

private:
    EventLoop loop_;
    std::unique_ptr<Acceptor> acceptor_;            // 监听组件 单和主形态由工厂 create 触发
    std::unique_ptr<ConnHandler> conn_handler_;     // 连接处理器 单和从属形态由工厂 create 触发
    std::function<void(int)> fd_handler_;           // 构造给兜底 主形态网关 set_fd_handler 覆盖 单从属 create_handler 覆盖
};
