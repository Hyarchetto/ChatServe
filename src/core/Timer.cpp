// Timer 实现 — 建 timerfd 挂进事件循环
#include "core/Timer.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cstdio>
#include <utility>

Timer::Timer(std::chrono::milliseconds interval, std::function<void()> cb)
    : interval_(interval), cb_(std::move(cb)) {}

bool Timer::attach(EventLoop& loop) {
    if (this->tick_.fd() >= 0) {
        return true;  // 已装过 幂等
    }
    // 零或负的周期在 timerfd 里是解除定时的语义 不是本类的意图
    if (this->interval_.count() <= 0) {
        return false;
    }
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        perror("timerfd_create");
        return false;
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(this->interval_).count();
    itimerspec spec{};
    spec.it_value.tv_sec = ns / 1000000000;         // 首次在一个周期之后
    spec.it_value.tv_nsec = ns % 1000000000;
    spec.it_interval = spec.it_value;               // 此后每周期一次
    if (timerfd_settime(fd, 0, &spec, nullptr) < 0) {
        perror("timerfd_settime");
        close(fd);
        return false;
    }
    return this->tick_.attach(loop, fd, [this]() { this->handle_tick(); });
}

// 到期回调 读干过期计数再跑一次
// 只跑一次不按计数循环 判据用的是绝对时间差 丢节拍只会让判定偏晚
void Timer::handle_tick() {
    this->tick_.drain();
    this->cb_();
}
