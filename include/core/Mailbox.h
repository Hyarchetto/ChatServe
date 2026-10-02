// 跨线程邮箱 — 自持唤醒 fd
#pragma once

#include <sys/eventfd.h>

#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

#include "BatchQueue.h"
#include "EventLoop.h"
#include "FdRegistration.h"

template <typename T>
class Mailbox {
public:
    // 回调只被消费线程执行，一次拿到整批，逐条 move 走内容
    // 返回后队列清空 ready_ 并留着重用，所以回调不得留存它的引用
    using Callback = typename BatchQueue<T>::Callback;

    explicit Mailbox(Callback callback) : queue_(std::move(callback)) {}

    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;

    // 建自己的唤醒 fd 并注册进事件循环
    bool attach(EventLoop& loop) {
        if (this->wake_.fd() >= 0) {
            return true;
        }
        const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) {
            perror("mailbox eventfd");
            return false;
        }
        return this->wake_.attach(loop, fd, [this]() { this->handle_wakeup(); });
    }

    // 追加单个，queue_自带锁，无需考虑线程安全
    void post(T item) {
        if (this->queue_.push(std::move(item))) {
            this->wake_.wakeup();
        }
    }

    // 追加一批
    void post_batch(std::vector<T> items) {
        if (this->queue_.push_batch(std::move(items))) {
            this->wake_.wakeup();
        }
    }

private:
    // 唤醒 fd 可读回调，只由消费线程执行，读干计数再排空
    void handle_wakeup() {
        this->wake_.drain();
        this->queue_.drain();
    }

    BatchQueue<T> queue_;       // 攒批交给它，本类只管唤醒自己
    FdRegistration wake_;       // 唤醒 fd 的注册
};
