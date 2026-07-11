// 主反应器的实现
// MainReactor 是整个服务器的启动入口和调度中心
//
// 初始化流程：
//   1. 创建指定数量的子 Reactor
//   2. 启动 Acceptor 开始监听端口
//   3. 启动所有子 Reactor 的事件循环线程
//   4. 主线程进入自己的事件循环
//
// 新连接到达时，MainReactor 根据当前各子 Reactor 的负载情况，
// 选择最空闲的一个来接管这个连接
#include "server/MainReactor.h"

#include <iostream>
#include <cstdio>

// 构造函数只保存参数，不做任何初始化
// 所有的初始化逻辑在 init 函数中延迟执行
MainReactor::MainReactor(int port, size_t sub_reactor_count)
    : port_(port), sub_reactor_count_(sub_reactor_count) {
}

// 析构时退出事件循环并关闭线程池
MainReactor::~MainReactor() {
    this->loop_.quit();
    this->works_.shutdown();
}

// 初始化主 Reactor 系统
//
// 具体步骤：
//   1. 初始化主线程的事件循环
//   2. 创建指定数量的子 Reactor
//      每个子 Reactor 共享同一个 ConnectionRouter、RoomManager 和 FileManager
//   3. 设置 Acceptor 的回调函数，并开始监听端口
//
// 返回 true 表示全部初始化成功
// 如果 Acceptor 启动失败则返回 false
bool MainReactor::init() {
    if (!this->loop_.init()) {
        return false;
    }

    for (size_t i = 0; i < this->sub_reactor_count_; ++i) {
        auto reactor = std::make_unique<Reactor>(
            &this->conn_router_,
            &this->room_mgr_,
            &this->file_mgr_);
        this->sub_reactors_.push_back(std::move(reactor));
    }

    this->acceptor_.set_new_connection_callback(
        [this](int fd) { this->on_new_connection(fd); });
    if (!this->acceptor_.start(this->port_, &this->loop_)) {
        return false;
    }

    std::cout << "服务器开始监听 " << this->port_ << std::endl;
    std::cout << "子 Reactor 数量: " << this->sub_reactor_count_ << std::endl;
    std::cout << "------------------------------------------" << std::endl;
    return true;
}

// 启动服务器
//
// 先启动所有子 Reactor 的事件循环线程，
// 每个子 Reactor 在独立线程中运行自己的 epoll_wait 循环
//
// 最后主线程进入自己的事件循环
// 主线程的事件循环主要处理 Acceptor 的 accept 事件
// 这个函数会一直阻塞直到服务器关闭
void MainReactor::start() {
    for (auto& reactor : this->sub_reactors_) {
        reactor->start();
    }

    this->loop_.loop();
}

// 新连接到达时的处理函数
//
// 当前采用最小负载优先的分发策略
// 比较所有子 Reactor 的待办回调队列长度，
// 选择 pending_size 最小的 Reactor 来接管这个新连接
//
// 这样做的原因是：pending_size 小的 Reactor 当前 IO 任务更少，
// 能更快地处理新连接的读写操作
//
// 相比纯粹的 round-robin 轮询策略，
// 这种负载感知分发能更好地平衡各 Reactor 的压力
void MainReactor::on_new_connection(int fd) {
    size_t best = 0;
    size_t min_load = sub_reactors_[0]->loop()->pending_size();

    for (size_t i = 1; i < sub_reactors_.size(); ++i) {
        size_t load = sub_reactors_[i]->loop()->pending_size();
        if (load < min_load) {
            min_load = load;
            best = i;
        }
    }

    sub_reactors_[best]->add_connection(fd);
}
