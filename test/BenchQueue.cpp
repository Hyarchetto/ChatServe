// BenchQueue — 线程池任务队列的互斥版与无锁版对拍
//
// 形状对齐线程池的真实访问，单生产者按批投递，多消费者取
// 两个池的 worker 逻辑与容量上限一致，差异只在队列与唤醒手段
//   互斥版  std::queue + mutex + condition_variable，即现状
//   无锁版  LockFreeQueue + mutex，锁只用来仲裁睡眠
//
// 量的是任务吞吐，即全部任务跑完的总数除以投递开始到跑完的时长
// 必须绑核跑，同机不绑核读数会被压低
//
// 用法 ./chat_queue_bench [工作线程数] [每批任务数] [总任务数] [任务空转轮数]
// 任务空转轮数用来调任务分量，从 0 逐档加大可以看出两者的交叉点在哪
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/LockFreeQueue.h"

namespace {

constexpr size_t kCapacity = 1024;

using Task = std::function<void()>;
using Clock = std::chrono::steady_clock;

// ==================== 互斥版，现状的写法 ====================

class MutexPool {
public:
    explicit MutexPool(size_t workers) {
        for (size_t i = 0; i < workers; ++i) {
            this->workers_.emplace_back([this]() { this->worker_loop(); });
        }
    }

    ~MutexPool() {
        {
            std::unique_lock<std::mutex> lock(this->mutex_);
            this->stop_ = true;
        }
        this->cv_.notify_all();
        for (std::thread& worker : this->workers_) {
            worker.join();
        }
    }

    bool try_post_batch(const std::vector<Task>& tasks) {
        {
            std::unique_lock<std::mutex> lock(this->mutex_);
            if (this->tasks_.size() + tasks.size() > kCapacity) {
                return false;
            }
            for (const Task& task : tasks) {
                this->tasks_.push(task);
            }
        }
        this->cv_.notify_all();
        return true;
    }

private:
    void worker_loop() {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(this->mutex_);
                this->cv_.wait(lock, [this] { return !this->tasks_.empty() || this->stop_; });
                if (this->tasks_.empty() && this->stop_) {
                    return;
                }
                task = std::move(this->tasks_.front());
                this->tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

// ==================== 无锁版 ====================

class LockFreePool {
public:
    explicit LockFreePool(size_t workers) {
        for (size_t i = 0; i < workers; ++i) {
            this->workers_.emplace_back([this]() { this->worker_loop(); });
        }
    }

    ~LockFreePool() {
        {
            std::unique_lock<std::mutex> lock(this->mutex_);
            this->stop_.store(true, std::memory_order_release);
        }
        this->cv_.notify_all();
        for (std::thread& worker : this->workers_) {
            worker.join();
        }
    }

    // 与 ThreadPool 的写法一致：推入与唤醒同在一把锁内，才能保证不丢唤醒
    bool try_post_batch(const std::vector<Task>& tasks) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        std::vector<Task> copy(tasks.begin(), tasks.end());
        if (!this->tasks_.try_push_batch(copy.data(), copy.size())) {
            return false;
        }
        this->cv_.notify_all();
        return true;
    }

private:
    void worker_loop() {
        while (true) {
            Task task;
            if (this->tasks_.try_pop(task)) {
                task();
                continue;
            }
            std::unique_lock<std::mutex> lock(this->mutex_);
            if (this->tasks_.try_pop(task)) {   // 进锁前又有人推了
                lock.unlock();
                task();
                continue;
            }
            if (this->stop_.load(std::memory_order_acquire)) {
                return;
            }
            this->cv_.wait(lock);
        }
    }

    std::vector<std::thread> workers_;
    LockFreeQueue<Task, kCapacity> tasks_;
    std::mutex mutex_;                       // 只仲裁睡眠，不保护数据
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};

// ==================== 对拍 ====================

template <typename Pool>
double run_once(size_t workers, size_t batch_size, size_t total, size_t cost) {
    std::atomic<uint64_t> sink{0};
    std::atomic<uint64_t> digest{0};
    Pool pool(workers);

    // cost 是任务体的空转轮数，用来模拟真实任务的分量
    // 太便宜的任务会让生产者一直追着消费者跑，把互斥版的惊群代价放大到失真
    std::vector<Task> batch(batch_size);
    for (Task& task : batch) {
        task = [&sink, &digest, cost]() {
            uint64_t x = cost + 1;
            for (size_t i = 0; i < cost; ++i) {
                x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            }
            sink.fetch_add(1, std::memory_order_relaxed);
            if (x == 0x5DEECE66DULL) {          // 几乎不会命中，只是拦住编译器把循环删掉
                digest.fetch_add(1, std::memory_order_relaxed);
            }
        };
    }

    const size_t batches = total / batch_size;
    const size_t expected = batches * batch_size;
    auto start = Clock::now();
    for (size_t b = 0; b < batches; ++b) {
        while (!pool.try_post_batch(batch)) {
            std::this_thread::yield();
        }
    }
    while (sink.load(std::memory_order_relaxed) < expected) {
        std::this_thread::yield();
    }
    auto finish = Clock::now();

    // 交付量必须分毫不差，少了说明丢任务，多了说明有重复
    const uint64_t delivered = sink.load(std::memory_order_relaxed);
    if (delivered != expected) {
        std::printf("  交付量不符 期望 %zu 实得 %llu\n", expected,
                    static_cast<unsigned long long>(delivered));
        std::abort();
    }
    return std::chrono::duration<double>(finish - start).count();
}

}  // namespace

int main(int argc, char** argv) {
    const size_t workers = argc > 1 ? static_cast<size_t>(std::atoi(argv[1])) : 4;
    const size_t batch_size = argc > 2 ? static_cast<size_t>(std::atoi(argv[2])) : 32;
    const size_t total = argc > 3 ? static_cast<size_t>(std::atoi(argv[3])) : 2000000;
    const size_t cost = argc > 4 ? static_cast<size_t>(std::atoi(argv[4])) : 0;

    // 一批比整个队列还大就永远投不进去，两个池都会在重试里空转
    if (workers == 0 || batch_size == 0 || batch_size > kCapacity || total < batch_size) {
        std::printf("参数不合法 每批须在 1..%zu 之间且总任务不少于一批\n", kCapacity);
        return 1;
    }

    std::printf("工作线程 %zu  每批 %zu  总任务 %zu  队列容量 %zu  空转 %zu 轮\n\n",
                workers, batch_size, total, kCapacity, cost);

    // 预热，让首个池不吃到冷启动
    run_once<MutexPool>(workers, batch_size, total / 10, cost);
    run_once<LockFreePool>(workers, batch_size, total / 10, cost);

    for (int round = 0; round < 2; ++round) {
        const double mutex_sec = run_once<MutexPool>(workers, batch_size, total, cost);
        const double lockfree_sec = run_once<LockFreePool>(workers, batch_size, total, cost);
        const double mutex_rate = static_cast<double>(total) / mutex_sec;
        const double lockfree_rate = static_cast<double>(total) / lockfree_sec;

        std::printf("第 %d 轮\n", round + 1);
        std::printf("  互斥版  %7.3f s  %8.2f 万/s\n", mutex_sec, mutex_rate / 10000.0);
        std::printf("  无锁版  %7.3f s  %8.2f 万/s\n", lockfree_sec, lockfree_rate / 10000.0);
        std::printf("  相对    %+7.1f%%\n\n", (lockfree_rate / mutex_rate - 1.0) * 100.0);
    }
    return 0;
}
