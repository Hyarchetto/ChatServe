// 线程池 — 固定工作线程，任务队列进出不加锁
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "LockFreeQueue.h"

class ThreadPool {
public:
    // 默认4条工作线程
    static constexpr size_t kDefaultThreadNum = 4;

    ThreadPool(size_t thread_num = kDefaultThreadNum);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 投递一批任务，全有或全无，放不下则一条都不进并抛异常
    void post_batch(std::vector<std::function<void()>> tasks);

    void shutdown();

private:
    static constexpr size_t kMaxTaskNum = 1024;

    std::vector<std::thread> workers_;
    LockFreeQueue<std::function<void()>, kMaxTaskNum> tasks_;
    std::mutex mutex_;                   // 只仲裁睡眠，不保护队列
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};

    void worker_loop(size_t index);
};
