// FdRegistration 实现 — 一条 fd 的注册 摘除与计数读写
#include "core/FdRegistration.h"
#include "core/EventLoop.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <utility>

FdRegistration::~FdRegistration() {
    this->detach();
}

bool FdRegistration::attach(EventLoop& loop, int fd, std::function<void()> on_ready) {
    if (this->fd_ >= 0) {
        return true;  // 已接管过 幂等
    }
    if (!loop.add_event(fd, EPOLLIN, std::move(on_ready))) {
        close(fd);  // 注册不上 fd 无人接手 由本类收尾
        return false;
    }
    this->fd_ = fd;
    this->loop_ = &loop;
    return true;
}

// 摘除要碰事件表与 epoll 实例 两者都还在的时候才做得了 所以由持有者挑时机而不是只等析构
void FdRegistration::detach() {
    if (this->fd_ < 0) {
        return;
    }
    this->loop_->del_event(this->fd_);
    close(this->fd_);
    this->fd_ = -1;
    this->loop_ = nullptr;
}

void FdRegistration::wakeup() {
    if (this->fd_ < 0) {
        return;
    }
    uint64_t count = 1;
    if (write(this->fd_, &count, sizeof(count)) < 0 && errno != EAGAIN) {
        perror("write wakeup fd");
    }
}

void FdRegistration::drain() {
    if (this->fd_ < 0) {
        return;
    }
    uint64_t count = 0;
    if (read(this->fd_, &count, sizeof(count)) < 0 && errno != EAGAIN) {
        perror("read wakeup fd");
    }
}

