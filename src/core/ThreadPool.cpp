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
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->stop_.store(true, std::memory_order_release);
    }
    // 每个 worker 都要醒来看到 stop_ 才肯退，唤醒方只叫一个会卡在 join 上
    this->cv_.notify_all();
    for (std::thread& worker : this->workers_) {
        if (worker.joinable()) worker.join();
    }
}

// 整批一次入队
void ThreadPool::post_batch(std::vector<std::function<void()>> tasks) {
    if (tasks.empty()) {
        return;
    }
    // 与 shutdown 共用这把锁，保证置位之后不再有任务进队
    // 推入与唤醒同在这一段里，才不会有“推完了却没叫人”的窗口
    // 每批只取一次，稳态下 worker 不睡，锁也就只在投递方手里过一下
    std::unique_lock<std::mutex> lock(this->mutex_);
    if (this->stop_.load(std::memory_order_acquire)) {
        throw std::runtime_error("线程池已失效");
    }
    if (!this->tasks_.try_push_batch(tasks.data(), tasks.size())) {
        throw std::runtime_error("任务队列已满");
    }
    // 一批可能够多条 worker 分头跑，全体唤醒只花一次系统调用
    this->cv_.notify_all();
}

void ThreadPool::worker_loop(size_t index) {
    while (true) {
        std::function<void()> task;
        // 先在锁外取，取到就直接跑，有活的时候碰不到这把锁
        if (!this->tasks_.try_pop(task)) {
            std::unique_lock<std::mutex> lock(this->mutex_);
            // 进锁前可能又有人推了，锁内再取一次再决定睡不睡
            if (!this->tasks_.try_pop(task)) {
                if (this->stop_.load(std::memory_order_acquire)) {
                    return;
                }
                this->cv_.wait(lock);
                continue;
            }
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
