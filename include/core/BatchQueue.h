// BatchQueue — 跨线程单消费者队列 攒批交出
// 生产方追加不等 消费方一次拿走整批 两个队列互换容量留住 稳态下不产生分配
// 入队序即 FIFO 单生产者对单消费者的顺序经此保持
//
// draining_ 记着消费方的排空是否在途 在途时生产方只追加不再唤醒
// 本类只负责攒批 唤醒手段由持有它的组件决定 队列本身不认识事件循环
#pragma once

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

template <typename T>
class BatchQueue {
public:
    // 回调只被消费线程执行 一次拿到整批 逐条 move 走内容
    using Callback = std::function<void(std::vector<T>&)>;

    explicit BatchQueue(Callback callback) : callback_(std::move(callback)) {}

    BatchQueue(const BatchQueue&) = delete;
    BatchQueue& operator=(const BatchQueue&) = delete;

    // 追加一条
    bool push(T item) {
        std::lock_guard<std::mutex> lock(this->mtx_);
        this->waiting_.push_back(std::move(item));
        return this->mark_draining();
    }

    // 追加一批
    bool push_batch(std::vector<T> items) {
        if (items.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(this->mtx_);
        this->waiting_.insert(this->waiting_.end(),
                              std::make_move_iterator(items.begin()),
                              std::make_move_iterator(items.end()));
        return this->mark_draining();
    }

    // 排空直到等待队列也完全完成
    void drain() {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(this->mtx_);
                if (this->waiting_.empty()) {
                    this->draining_ = false;
                    return;
                }
                this->ready_.swap(this->waiting_);
            }
            this->callback_(this->ready_);
            this->ready_.clear();
        }
    }

private:
    // 锁内调用，查询是否需要唤醒
    bool mark_draining() {
        bool need_wake = !this->draining_;
        this->draining_ = true;
        return need_wake;
    }

    Callback callback_;
    std::mutex mtx_;
    std::vector<T> ready_;          // 就绪队列 只消费线程碰
    std::vector<T> waiting_;        // 等待队列 生产方追加 锁保护
    bool draining_ = false;         // 有排空在途 锁保护
};
