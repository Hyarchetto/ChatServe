// Timer 用例 — 周期节拍与装配守卫
#include "TestMain.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "core/EventLoop.h"
#include "core/Timer.h"

TEST(timer_fires_every_interval) {
    EventLoop loop;
    CHECK(loop.init());
    std::atomic<int> ticks{0};
    Timer timer(std::chrono::milliseconds(20), [&ticks]() { ticks.fetch_add(1); });
    CHECK(timer.attach(loop));

    std::thread runner([&loop]() { loop.loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    loop.quit();
    runner.join();

    // 200ms 二十毫秒一拍应在十次上下 下界放宽到负载抖动
    CHECK(ticks.load() >= 3);
    // 回调漏读 timerfd 会让 epoll 反复就绪 计数飙到几十万 上界同时守住自旋
    CHECK(ticks.load() <= 15);
}

TEST(timer_rejects_non_positive_interval) {
    EventLoop loop;
    CHECK(loop.init());
    Timer timer(std::chrono::milliseconds(0), []() {});
    CHECK(!timer.attach(loop));
}

TEST(timer_attach_is_idempotent) {
    EventLoop loop;
    CHECK(loop.init());
    Timer timer(std::chrono::milliseconds(20), []() {});
    CHECK(timer.attach(loop));
    CHECK(timer.attach(loop));
}
