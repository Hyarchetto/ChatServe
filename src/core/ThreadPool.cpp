// 线程池
#include "core/ThreadPool.h"

#include <iostream>

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
    this->condition_.notify_all();
    for (std::thread& worker : this->workers_) {
        if (worker.joinable()) worker.join();
    }
}

void ThreadPool::shutdown_now() {
    {
        std::unique_lock<std::mutex> lock(this->queue_mutex_);
        this->stop_.store(true, std::memory_order_release);
        while (!this->tasks_.empty()) this->tasks_.pop();
    }
    this->condition_.notify_all();
    for (std::thread& worker : this->workers_) {
        if (worker.joinable()) worker.join();
    }
}

size_t ThreadPool::get_thread_num() const {
    return this->workers_.size();
}

size_t ThreadPool::task_count() const {
    std::lock_guard<std::mutex> lock(this->queue_mutex_);
    return this->tasks_.size();
}

bool ThreadPool::is_running() const {
    return !this->stop_.load(std::memory_order_acquire);
}

void ThreadPool::wait_all() {
    std::unique_lock<std::mutex> lock(this->queue_mutex_);
    // stop_ 一并作为出口，否则池停之后 worker 全退，条件再满足也没人唤醒
    // 取活与减 active_tasks_ 都在本锁内，锁内观察到的计数与队列自洽
    this->condition_.wait(lock, [this] {
        return this->stop_.load(std::memory_order_acquire) ||
               (this->tasks_.empty() && this->active_tasks_ == 0);
    });
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
            ++this->active_tasks_;
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
        {
            std::lock_guard<std::mutex> lock(this->queue_mutex_);
            --this->active_tasks_;
            if (this->tasks_.empty() && this->active_tasks_ == 0) {
                this->condition_.notify_all();
            }
        }
    }
}
