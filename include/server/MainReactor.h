// 主反应器
// 这是整个服务器的核心调度器，采用经典的主从 Reactor 架构
//
// 主 Reactor 负责做两件事：
//   1. 在一个独立线程中通过 Acceptor 不断 accept 新连接
//   2. 把 accept 出来的新连接分发给各个子 Reactor
//
// 子 Reactor 负责管理具体的客户端连接，处理真正的 IO 操作
// 这种设计把连接接收和 IO 处理分开，避免了单线程瓶颈
#pragma once

#include <vector>
#include <memory>
#include <cstddef>

#include "../core/ThreadPool.h"
#include "../core/EventLoop.h"
#include "Acceptor.h"
#include "ConnectionRouter.h"
#include "Reactor.h"
#include "../chatroom/Room.h"
#include "../filetransfer/FileManager.h"

class MainReactor {
public:
    // 构造一个主 Reactor
    // port 是监听端口号
    // sub_reactor_count 是子 Reactor 的数量
    MainReactor(int port, size_t sub_reactor_count);
    ~MainReactor();

    // 初始化所有子 Reactor、启动 Acceptor
    // 在子 Reactor 启动之前必须先调用本函数
    // 返回 true 表示初始化成功
    bool init();

    // 启动所有子 Reactor 的事件循环线程
    // 然后主线程进入自己的事件循环
    // 这个函数会一直阻塞直到服务器关闭
    void start();

private:
    int port_;                     // 监听端口
    size_t sub_reactor_count_;     // 子 Reactor 的数量

    // 主 Reactor 自己的事件循环
    // 在这个循环里 Acceptor 监听新连接
    EventLoop loop_;

    // 连接接收器，负责 accept 新连接
    Acceptor acceptor_;

    // 主 Reactor 的线程池
    // 目前暂时没有被使用，保留供后续扩展
    ThreadPool works_;

    // 连接路由表
    // 所有子 Reactor 共享同一个实例，用来跨 Reactor 消息路由
    ConnectionRouter conn_router_;

    // 房间管理器
    // 所有子 Reactor 共享同一个实例
    RoomManager room_mgr_;

    // 文件管理器
    // 所有子 Reactor 共享同一个实例
    FileManager file_mgr_;

    // 子 Reactor 的指针数组
    // 每个子 Reactor 在独立的线程中运行自己的事件循环
    std::vector<std::unique_ptr<Reactor>> sub_reactors_;

    // 轮询分配用的计数器
    // 每 accept 一个连接就递增并取模，实现 round-robin 分发
    size_t next_reactor_ = 0;

    // 新连接到达时的处理函数
    // 选择一个合适的子 Reactor 并把连接交过去
    void on_new_connection(int fd);
};
