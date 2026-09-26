// 线程池
#include "core/ThreadPool.h"

#include <iostream>
#include <stdexcept>
#include <utility>

ThreadPool::ThreadPool(size_t thread_num) {
    if (thread_num == 0) {
        thread_num = 1;
    }
    for (size_t i = 0; i < thread_num; ++i) {
        this->workers_.emplace_back([this, i] { this->worker_loop(i); });
    }
}

ThreadPool::~ThreadPool() {
    this->shutdown();
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(this->queue_mutex_);
        this->stop_.store(true, std::memory_order_release);
    }
    // 每个 worker 都要醒来看到 stop_ 才肯退，唤醒方只叫一个会卡在 join 上
    this->condition_.notify_all();
    for (std::thread& worker : this->workers_) {
        if (worker.joinable()) worker.join();
    }
}

// 整批一次入队 一次唤醒 
void ThreadPool::post_batch(std::vector<std::function<void()>> tasks) {
    if (tasks.empty()) {
        return;
    }
    {
        std::unique_lock<std::mutex> lock(this->queue_mutex_);
        // 两道门都在主流程之前，放行后才入队
        if (this->stop_.load(std::memory_order_acquire)) {
            throw std::runtime_error("线程池已失效");
        }
        if (this->tasks_.size() + tasks.size() > this->kMaxTaskNum) {
            throw std::runtime_error("任务队列已满");
        }
        for (auto& task : tasks) {
            this->tasks_.push(std::move(task));
        }
    }
    // 一批可能够多条 worker 分头跑，全体唤醒只花一次系统调用
    this->condition_.notify_all();
}

void ThreadPool::worker_loop(size_t index) {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(this->queue_mutex_);
            this->condition_.wait(lock, [this] {
                return !this->tasks_.empty() || this->stop_.load(std::memory_order_acquire);
            });

            if (this->tasks_.empty() && this->stop_.load(std::memory_order_acquire)) {
                return;
            }
            task = std::move(this->tasks_.front());
            this->tasks_.pop();
        }
        try {
            task();
        }
        catch (const std::exception& e) {
            std::cerr << "ThreadPool: worker-" << index
                      << " 任务捕获异常: " << e.what() << std::endl;
        }
        catch (...) {
            std::cerr << "ThreadPool: worker-" << index
                      << " 任务捕获未知异常" << std::endl;
        }
    }
}
