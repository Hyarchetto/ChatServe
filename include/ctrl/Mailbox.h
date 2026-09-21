// 跨线程单消费者邮箱 — 归一条消费线程的 EventLoop
// 两个队列互换，生产方只追加不等，消费方拿整批
//   waiting_ 等待队列 生产方一直往这追加，消费方在处理时到达的全落这里
//   ready_   就绪队列 攒齐了交给 sink 一整批处理的那条
// drain 时两个队列互换 ready_ 换上刚攒好的 waiting_ waiting_ 换成刚清空的缓冲
// 两边容量都留住，稳态下不产生分配
//
// draining_ 记着消费方的排空是否在途，在途时生产方只追加不再唤醒
// 入队序即 FIFO 单生产者对单 fd 的消息顺序经此保持
#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "../core/EventLoop.h"

template <typename T>
class Mailbox {
public:
    // sink 只被消费线程执行 一次拿到整批 逐条 move 走内容
    // 返回后本类清空 ready_ 并留着重用 所以 sink 不得留存它的引用
    using Sink = std::function<void(std::vector<T>&)>;

    Mailbox(EventLoop& consumer, Sink sink)
        : consumer_(consumer), sink_(std::move(sink)) {}

    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;

    // 追加单个 线程安全
    void post(T item) {
        bool need_wake = false;
        {
            std::lock_guard<std::mutex> lock(this->mtx_);
            this->waiting_.push_back(std::move(item));
            need_wake = this->mark_draining();
        }
        if (need_wake) {
            this->consumer_.post([this]() { this->drain(); });
        }
    }

    // 追加一批 线程安全 整批只加一次锁只唤醒一次
    void post_batch(std::vector<T> items) {
        if (items.empty()) {
            return;
        }
        bool need_wake = false;
        {
            std::lock_guard<std::mutex> lock(this->mtx_);
            this->waiting_.insert(this->waiting_.end(),
                                  std::make_move_iterator(items.begin()),
                                  std::make_move_iterator(items.end()));
            need_wake = this->mark_draining();
        }
        if (need_wake) {
            this->consumer_.post([this]() { this->drain(); });
        }
    }

private:
    // 锁内调用 置位并回答是否需要唤醒
    bool mark_draining() {
        bool need_wake = !this->draining_;
        this->draining_ = true;
        return need_wake;
    }

    // 只在消费线程执行 排空到等待队列空再放行唤醒 追加晚于交换的由下一轮收
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
            this->sink_(this->ready_);
            this->ready_.clear();
        }
    }

    EventLoop& consumer_;
    Sink sink_;
    std::vector<T> ready_;          // 就绪队列 只消费线程碰
    std::vector<T> waiting_;        // 等待队列 生产方追加 锁保护
    std::mutex mtx_;
    bool draining_ = false;         // 有排空在途 锁保护
};
