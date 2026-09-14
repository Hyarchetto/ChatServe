// ReactorFactory — Reactor 形态与服务器工厂实现
#include "server/ReactorFactory.h"
#include "server/Reactor.h"
#include "server/CtrlDispatcher.h"
#include "core/ThreadPool.h"

#include <vector>

ReactorFactory::ReactorFactory() {
    // 业务线程池与中控为公共资源 归工厂持有 线程池随工厂生成即拉起
    this->works_ = std::make_unique<ThreadPool>();
    this->dispatcher_ = std::make_unique<CtrlDispatcher>(*this->works_);
}

ReactorFactory::~ReactorFactory() {
    this->shutdown();
}

// 单 Reactor 同时建监听与连接处理 业务走中控 io 序号 0
std::unique_ptr<Reactor> ReactorFactory::create_single() {
    size_t io = this->next_io_++;
    auto reactor = std::make_unique<Reactor>();
    reactor->create_acceptor();
    reactor->create_handler(static_cast<int>(io), this->dispatcher_->inbox());
    this->dispatcher_->attach_outbox(io, reactor->outbox());
    this->start_dispatcher();
    return reactor;
}

std::unique_ptr<Reactor> ReactorFactory::create_main() {
    auto reactor = std::make_unique<Reactor>();
    reactor->create_acceptor();
    return reactor;
}

// io 从属 Reactor 绑定中控上行收件箱 并把自身下行出站挂到中控
std::unique_ptr<Reactor> ReactorFactory::create_sub() {
    size_t io = this->next_io_++;
    auto reactor = std::make_unique<Reactor>();
    reactor->create_handler(static_cast<int>(io), this->dispatcher_->inbox());
    this->dispatcher_->attach_outbox(io, reactor->outbox());
    return reactor;
}

// 生成主从工作者服务器 用 create_main/create_sub 产组件并组装
std::unique_ptr<Gateway> ReactorFactory::create_gateway(size_t sub_count) {
    this->next_io_ = 0;
    auto main = this->create_main();
    std::vector<std::unique_ptr<Reactor>> subs;
    subs.reserve(sub_count);
    for (size_t i = 0; i < sub_count; ++i) {
        subs.push_back(this->create_sub());
    }
    this->start_dispatcher();
    return std::make_unique<Gateway>(std::move(main), std::move(subs));
}

// 停中控线程后排空业务线程池 保证在途任务完成前工厂对象仍存活
void ReactorFactory::shutdown() {
    this->dispatcher_->stop();
    this->works_->shutdown();
}

void ReactorFactory::start_dispatcher() {
    this->dispatcher_->start();
}
