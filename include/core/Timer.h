// Timer — 周期定时器，自持 timerfd 注册到事件循环，每 interval 回调一次
#pragma once

#include <chrono>
#include <functional>

#include "EventLoop.h"
#include "FdRegistration.h"

class Timer {
public:
    // cb 只被 loop 线程执行，每次到点调一次
    Timer(std::chrono::milliseconds interval, std::function<void()> cb);

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    // 建 timerfd 设周期并注册进事件循环
    // 须在 loop.init() 之后 loop.loop() 之前调用，周期非正或注册失败返回 false
    bool attach(EventLoop& loop);

private:
    // fd 可读回调，只由 loop 线程执行，读干过期计数再跑一次
    void handle_tick();

    std::chrono::milliseconds interval_;   // 周期，首次触发在一个周期之后
    std::function<void()> cb_;
    FdRegistration tick_;                  // timerfd 的注册
};
