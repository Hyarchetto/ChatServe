// 线程池 — 固定工作线程，任务队列
#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

class ThreadPool {
public:
    // 默认 4 条
    static constexpr size_t kDefaultThreadNum = 4;

    ThreadPool(size_t thread_num = kDefaultThreadNum);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 投递一批任务，整批只加一次队列锁、只唤醒一次
    void post_batch(std::vector<std::function<void()>> tasks);

    void shutdown();

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;

    static constexpr int kMaxTaskNum = 1024;
    std::atomic<bool> stop_{false};

    void worker_loop(size_t index);
};
