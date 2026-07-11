// 线程池 — 固定工作线程，任务队列
#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <iostream>

class ThreadPool {
public:
    ThreadPool(size_t thread_num = std::thread::hardware_concurrency());
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 提交任务，返回 future
    template<class F, class ...Args>
    std::future<typename std::invoke_result_t<F, Args...>> submit(F&& f, Args&&... args);

    // 非阻塞提交，失败返回 false
    template<class F, class ...Args>
    bool try_submit(F&& f, Args&&... args);

    void shutdown();
    void shutdown_now();

    size_t get_thread_num() const;
    size_t task_count() const;
    bool is_running() const;
    void wait_all();

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;

    static constexpr int max_task_num_ = 1024;
    std::atomic<bool> stop_{false};
    size_t active_tasks_{0};

    void worker_loop(size_t index);
};

// ==================== 模板实现 ====================

template<class F, class ...Args>
std::future<typename std::invoke_result_t<F, Args...>> ThreadPool::submit(F&& f, Args&&... args) {
    using return_type = typename std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        [f = std::forward<F>(f), args = std::make_tuple(std::forward<Args>(args)...)]() mutable
            -> return_type {
            return std::apply(std::move(f), std::move(args));
        }
    );
    std::future<return_type> res = task->get_future();

    {
        std::unique_lock<std::mutex> lock(this->queue_mutex_);
        if (this->stop_.load(std::memory_order_acquire)) {
            throw std::runtime_error("线程池已失效");
        }
        if (this->tasks_.size() < this->max_task_num_) {
            this->tasks_.emplace([task = std::move(task)]() mutable { (*task)(); });
        } else {
            throw std::runtime_error("任务队列已满");
        }
    }
    this->condition_.notify_one();
    return res;
}

template<class F, class ...Args>
bool ThreadPool::try_submit(F&& f, Args&&... args) {
    using return_type = typename std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        [f = std::forward<F>(f), args = std::make_tuple(std::forward<Args>(args)...)]() mutable
            -> return_type {
            return std::apply(std::move(f), std::move(args));
        }
    );

    {
        std::unique_lock<std::mutex> lock(this->queue_mutex_);
        if (this->stop_.load(std::memory_order_acquire) || this->tasks_.size() >= this->max_task_num_) {
            return false;
        }
        this->tasks_.emplace([task = std::move(task)]() mutable { (*task)(); });
    }
    this->condition_.notify_one();
    return true;
}
