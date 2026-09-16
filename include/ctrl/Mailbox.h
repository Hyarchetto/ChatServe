// 跨线程单消费者邮箱 — 归一条消费线程的 EventLoop
// 生产线程 post/post_batch 追加并统一唤醒一次 消费线程 drain 排空到无再放行新唤醒
// 一次 post 排空整批 避免逐条唤醒 跨线程唤醒的开销只付一次
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
    // sink 只被消费线程执行 按入队序逐条调用
    using Sink = std::function<void(T)>;

    Mailbox(EventLoop& consumer, Sink sink)
        : consumer_(consumer), sink_(std::move(sink)) {}

    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;

    // 追加单个 线程安全
    void post(T item) {
        this->append_and_wake(std::move(item));
    }

    // 追加一批 线程安全 只保证一次唤醒
    void post_batch(std::vector<T> items) {
        for (auto& it : items) {
            this->append_and_wake(std::move(it));
        }
    }

private:
    // 队列非空且尚无排空在途时置位并唤醒 否则只追加 由在途排空兜底
    void append_and_wake(T item) {
        bool need_wake = false;
        {
            std::lock_guard<std::mutex> lock(this->mtx_);
            this->q_.push_back(std::move(item));
            if (!this->scheduled_) {
                this->scheduled_ = true;
                need_wake = true;
            }
        }
        if (need_wake) {
            this->consumer_.post([this]() { this->drain(); });
        }
    }

    // 只在消费线程执行 排空到队列空再放行唤醒 追加晚于交换的由下一轮收
    void drain() {
        while (true) {
            std::deque<T> local;
            {
                std::lock_guard<std::mutex> lock(this->mtx_);
                if (this->q_.empty()) {
                    this->scheduled_ = false;
                    return;
                }
                this->q_.swap(local);
            }
            for (auto& item : local) {
                this->sink_(std::move(item));
            }
        }
    }

    EventLoop& consumer_;
    Sink sink_;
    std::deque<T> q_;               // 待消费队列 锁保护
    std::mutex mtx_;
    bool scheduled_ = false;        // 有排空在途 锁保护
};
