// Gateway — 主从服务器组装层实现
#include "server/Gateway.h"

#include <unistd.h>

Gateway::Gateway(std::unique_ptr<Reactor> main,
                 std::vector<std::unique_ptr<Reactor>> subs)
    : main_(std::move(main)) {
    this->subs_.reserve(subs.size());
    for (auto& sub : subs) {
        this->subs_.push_back(SubUnit{std::move(sub), {}});
    }
    // 主 Reactor 的 fd 去路改为网关分发 从属 io Reactor 在 create_sub 已绑本地处理
    this->main_->set_fd_handler([this](int fd) { this->dispatch_fd(fd); });
}

Gateway::~Gateway() {
    // 兜底 正常流程 loop 已在返回前收齐 io 线程
    this->join();
}

bool Gateway::init() {
    if (!this->main_->init()) {
        return false;
    }
    for (auto& s : this->subs_) {
        if (!s.reactor->init()) {
            return false;
        }
    }
    return true;
}

// 主 Reactor 监听 子线程推迟到 loop 再拉起 失败时无线程残留
bool Gateway::start_listen(int port) {
    return this->main_->start_listen(port);
}

void Gateway::loop() {
    // 进主循环前先拉起 io Reactor 线程 保证 accept 分发 fd 时 io loop 已在跑
    for (auto& s : this->subs_) {
        s.thread = std::thread([reactor = s.reactor.get()]() { reactor->loop(); });
    }
    // 阻塞到 stop 信号 quit 主 loop
    this->main_->loop();
    // stop 已把 io 线程 quit 收齐后再返回
    this->join();
}

void Gateway::stop() {
    this->main_->stop();
    for (auto& s : this->subs_) {
        s.reactor->stop();
    }
}

// 等待 io Reactor 线程退出 loop 尾部与析构兜底共用 可重复调用
void Gateway::join() {
    for (auto& s : this->subs_) {
        if (s.thread.joinable()) {
            s.thread.join();
        }
    }
}

// 主 Reactor accept 到的 fd 按 fd 哈希分发 只主 loop 线程调用
void Gateway::dispatch_fd(int fd) {
    if (this->subs_.empty()) {
        // 构造契约保证至少一个子 兜底关闭避免 fd 泄漏
        close(fd);
        return;
    }
    size_t idx = static_cast<size_t>(fd) % this->subs_.size();
    this->subs_[idx].reactor->add_connection(fd);
}
