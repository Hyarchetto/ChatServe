// ReactorFactory — Reactor 形态与网关工厂
#include "server/ReactorFactory.h"
#include "server/Gateway.h"

#include <vector>

std::unique_ptr<Reactor> ReactorFactory::create_single() {
    auto reactor = std::make_unique<Reactor>();
    reactor->create_acceptor();
    reactor->create_handler(this->works_, this->room_mgr_);
    return reactor;
}

std::unique_ptr<Reactor> ReactorFactory::create_main() {
    auto reactor = std::make_unique<Reactor>();
    reactor->create_acceptor();
    return reactor;
}

std::unique_ptr<Reactor> ReactorFactory::create_sub() {
    auto reactor = std::make_unique<Reactor>();
    reactor->create_handler(this->works_, this->room_mgr_);
    return reactor;
}

// 生成主从网关服务器 工厂产主/子 Reactor 组件并组装 网关本身不持工厂
std::unique_ptr<Gateway> ReactorFactory::create_gateway(size_t sub_count) {
    // 至少一个子 Reactor 防除零
    if (sub_count == 0) {
        sub_count = 1;
    }
    auto main = this->create_main();
    std::vector<std::unique_ptr<Reactor>> subs;
    for (size_t i = 0; i < sub_count; ++i) {
        subs.push_back(this->create_sub());
    }
    return std::make_unique<Gateway>(std::move(main), std::move(subs));
}
