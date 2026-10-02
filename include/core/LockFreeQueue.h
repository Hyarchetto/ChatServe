// LockFreeQueue — 全程无锁，但不保证公平，也不保证 FIFO 之外的任何顺序承诺
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

template <typename T, size_t N>
class LockFreeQueue {
    static_assert(N > 0 && (N & (N - 1)) == 0, "无锁队列的长度必须是2的幂");
    // 写入中途抛出会让位置越过槽位而序号永不推进，队列从此楔死
    static_assert(std::is_nothrow_move_assignable<T>::value,
                  "T 的移动赋值必须 noexcept 否则队列可能永久卡死");
    static_assert(std::is_nothrow_move_constructible<T>::value,
                  "T 的移动构造必须 noexcept");

public:
    LockFreeQueue() {
        for (size_t i = 0; i < N; ++i) {
            this->cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    // 推入一条，队满返回 false
    bool try_push(T value) {
        size_t pos = this->enqueue_pos_.load(std::memory_order_relaxed);
        while (true) {
            Cell& cell = this->cells_[pos & kMask];
            size_t seq = cell.seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                if (this->enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_acq_rel, 
                                                             std::memory_order_acquire)) {
                    cell.data = std::move(value);
                    cell.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0) {
                return false;                       // 这一格还没绕回来，满
            }
            else {
                pos = this->enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    // 取走一条，队列空返回 false
    bool try_pop(T& out) {
        size_t pos = this->dequeue_pos_.load(std::memory_order_relaxed);
        while (true) {
            Cell& cell = this->cells_[pos & kMask];
            size_t seq = cell.seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0) {
                if (this->dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    out = std::move(cell.data);
                    // 释放给绕一圈之后的位置 pos + N
                    cell.seq.store(pos + kMask + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0) {
                return false;                       // 这一格还没填，空
            }
            else {
                pos = this->dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    // 整批推入，全有或全无，放不下则一条都不推
    // 逐槽校验不能只看最后一个，因为取走一条要分三步，中间隔着一个可长可短的窗口
    // 先 CAS 推进位置，再读走数据，最后才 release 序号
    // 只看最后一格会漏掉前面尚有在途读取的格子，覆写掉还没被取走的数据
    bool try_push_batch(T* items, size_t count) {
        if (count == 0) {
            return true;
        }
        if (count > N) {
            return false;
        }
        size_t pos = this->enqueue_pos_.load(std::memory_order_relaxed);
        while (true) {
            bool stale = false;
            for (size_t i = 0; i < count; ++i) {
                size_t seq = this->cells_[(pos + i) & kMask].seq.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + i);
                if (diff < 0) {
                    return false;                   // 连续 count 格凑不齐
                }
                if (diff > 0) {
                    stale = true;                   // 位置落后，重读再查
                    break;
                }
            }
            if (stale) {
                pos = this->enqueue_pos_.load(std::memory_order_relaxed);
                continue;
            }
            if (!this->enqueue_pos_.compare_exchange_weak(pos, pos + count, std::memory_order_acq_rel, 
                                                          std::memory_order_acquire)) {
                continue;
            }
            for (size_t i = 0; i < count; ++i) {
                this->cells_[(pos + i) & kMask].data = std::move(items[i]);
            }
            // release 会发布本线程中所有先于它的写，所以两个循环分开是安全的
            // 逐槽写一个发布一个延迟更低，但正确性不需要
            for (size_t i = 0; i < count; ++i) {
                this->cells_[(pos + i) & kMask].seq.store(pos + i + 1, std::memory_order_release);
            }
            return true;
        }
    }

    static constexpr size_t capacity() { return N; }

private:
    static constexpr size_t kMask = N - 1;

    // 每槽独占一条缓存行，否则相邻槽位的序号操作互相伪共享
    struct alignas(64) Cell {
        std::atomic<size_t> seq;
        T data;
    };

    alignas(64) std::atomic<size_t> enqueue_pos_{0};
    alignas(64) std::atomic<size_t> dequeue_pos_{0};
    alignas(64) Cell cells_[N];
};
