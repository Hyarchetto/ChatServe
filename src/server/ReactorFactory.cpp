// ReactorFactory — Reactor 形态工厂
#include "server/ReactorFactory.h"

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
