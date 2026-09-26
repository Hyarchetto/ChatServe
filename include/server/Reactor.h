// Reactor — 统一事件循环服务器单元 一个类三形态 装配方按形态调用 create 生成
// 主 Reactor   监听 accept 把 fd 交给网关回调 由网关分发到从属 io 线程
// 从属 Reactor 不监听 网关把 fd 投递进来 本线程做 io worker 连接泵
// 主形态 create_acceptor 从属形态 create_handler 各自只建所需组件
// io worker 不碰业务状态 连接泵只读写自己的连接 业务经中控邮箱上下行
#pragma once

#include <functional>
#include <memory>

#include "Acceptor.h"
#include "../conn/ConnHandler.h"
#include "../ctrl/CtrlMsg.h"
#include "../core/Mailbox.h"
#include "../core/EventLoop.h"

class Reactor {
public:
    // 构造只建事件循环并给 fd 去路兜底 组件由装配方按形态 create 注入
    Reactor();
    ~Reactor();

    // 创建监听组件 Acceptor 主形态由装配方调用
    void create_acceptor();
    // 创建连接处理器并绑定 fd 去路 从属 io worker 形态由装配方调用
    // 上行邮箱与 io 序号注入 ConnHandler
    void create_handler(int io_index, Mailbox<CtrlUp>& ctrl_uplink_box);
    // 修改 fd 去路
    void set_fd_handler(std::function<void(int)> handler);
    // 创建 epoll 和 eventfd
    bool init();

    // 内部 Acceptor 监听端口 accept 到的 fd 走 fd_handler_ 成功返回 true
    bool start_listen(int port);

    // 从属入口 网关投递新连接 fd 内部切到本事件循环执行
    void add_connection(int fd);

    // 本 io 的下行邮箱 从属形态装配后供中控挂接
    Mailbox<CtrlDown>& downlink_box();

    // 事件循环主函数 阻塞直到服务器退出
    void loop();

    // 停止事件循环 quit 通过原子标志加eventfd唤醒
    void stop();

private:
    EventLoop loop_;
    std::unique_ptr<Acceptor> acceptor_;            // 监听组件 主形态由装配方 create 触发
    std::unique_ptr<ConnHandler> conn_handler_;     // 连接处理器 从属形态由装配方 create 触发
    std::function<void(int)> fd_handler_;           // 构造给兜底 主形态网关 set_fd_handler 覆盖
};
