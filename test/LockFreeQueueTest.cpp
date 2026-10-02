// LockFreeQueue 用例 — 顺序、边界、批量语义与多线程无丢失
#include "TestMain.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/LockFreeQueue.h"

TEST(lockfree_queue_empty_pop_fails) {
    LockFreeQueue<int, 8> queue;
    int value = 0;
    CHECK(!queue.try_pop(value));
}

TEST(lockfree_queue_preserves_fifo_order) {
    LockFreeQueue<int, 8> queue;
    for (int i = 0; i < 5; ++i) {
        CHECK(queue.try_push(i));
    }
    for (int i = 0; i < 5; ++i) {
        int value = -1;
        CHECK(queue.try_pop(value));
        CHECK_EQ(value, i);
    }
}

// 容量是 N 不是 N-1，这条容易开错，钉死它
TEST(lockfree_queue_capacity_is_exactly_n) {
    LockFreeQueue<int, 8> queue;
    CHECK_EQ(queue.capacity(), static_cast<size_t>(8));
    for (int i = 0; i < 8; ++i) {
        CHECK(queue.try_push(i));
    }
    CHECK(!queue.try_push(99));       // 第 9 条放不下
}

TEST(lockfree_queue_wraps_without_losing_values) {
    LockFreeQueue<int, 4> queue;
    int value = 0;
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 4; ++i) {
            CHECK(queue.try_push(round * 4 + i));
        }
        CHECK(!queue.try_push(-1));   // 绕回来之后照样是满的
        for (int i = 0; i < 4; ++i) {
            CHECK(queue.try_pop(value));
            CHECK_EQ(value, round * 4 + i);
        }
        CHECK(!queue.try_pop(value)); // 取空之后照样是空的
    }
}

TEST(lockfree_queue_batch_is_all_or_nothing) {
    LockFreeQueue<int, 8> queue;
    int items[8];
    for (int i = 0; i < 8; ++i) {
        items[i] = i;
    }

    CHECK(queue.try_push_batch(items, 5));
    int value = -1;
    for (int i = 0; i < 5; ++i) {
        CHECK(queue.try_pop(value));
        CHECK_EQ(value, i);
    }

    CHECK(queue.try_push_batch(items, 6));    // 占掉 6 格，只剩 2 格
    CHECK(!queue.try_push_batch(items, 3));   // 差一条，整批不进
    CHECK(!queue.try_push_batch(items, 3));   // 再来一次仍不进，说明上次没留副作用

    for (int i = 0; i < 6; ++i) {             // 队列内容原样，没被污染
        CHECK(queue.try_pop(value));
        CHECK_EQ(value, i);
    }
    CHECK(!queue.try_pop(value));
}

TEST(lockfree_queue_batch_rejects_oversized) {
    LockFreeQueue<int, 8> queue;
    int items[16] = {};
    CHECK(!queue.try_push_batch(items, 16));  // 超过容量直接拒
    CHECK(queue.try_push_batch(items, 8));    // 正好等于容量可以
    CHECK(!queue.try_push_batch(items, 1));   // 满了
}

// 多生产者多消费者，值不丢不重，总数与校验和对得上
TEST(lockfree_queue_multithreaded_loses_nothing) {
    constexpr size_t kProducers = 4;
    constexpr size_t kConsumers = 4;
    constexpr size_t kPerProducer = 50000;
    constexpr size_t kTotal = kProducers * kPerProducer;

    LockFreeQueue<uint64_t, 256> queue;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> sum{0};
    std::atomic<uint64_t> popped{0};

    std::vector<std::thread> producers;
    for (size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&queue, p]() {
            for (size_t i = 0; i < kPerProducer; ++i) {
                uint64_t value = p * kPerProducer + i;
                while (!queue.try_push(value)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::thread> consumers;
    for (size_t c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&]() {
            uint64_t value = 0;
            while (true) {
                if (queue.try_pop(value)) {
                    sum.fetch_add(value, std::memory_order_relaxed);
                    popped.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                // 空了，但生产者可能还在推，只有收齐全部推送量才收工
                if (done.load(std::memory_order_acquire) &&
                    popped.load(std::memory_order_relaxed) >= kTotal) {
                    break;
                }
                std::this_thread::yield();
            }
        });
    }

    for (std::thread& producer : producers) {
        producer.join();
    }
    done.store(true, std::memory_order_release);
    for (std::thread& consumer : consumers) {
        consumer.join();
    }

    uint64_t expected_sum = 0;
    for (size_t p = 0; p < kProducers; ++p) {
        for (size_t i = 0; i < kPerProducer; ++i) {
            expected_sum += p * kPerProducer + i;
        }
    }
    CHECK_EQ(popped.load(), static_cast<uint64_t>(kTotal));
    CHECK_EQ(sum.load(), expected_sum);
}

// 并发批量投递，批量预留是出错过的路径，必须也压在多线程下
// 队列刻意开得很小，逼生产者在接近满的状态下反复做逐槽校验与预留
TEST(lockfree_queue_multithreaded_batch_loses_nothing) {
    constexpr size_t kProducers = 2;
    constexpr size_t kConsumers = 4;
    constexpr size_t kBatch = 32;
    constexpr size_t kPerProducer = 40000;
    constexpr size_t kTotal = kProducers * kPerProducer;

    LockFreeQueue<uint64_t, 64> queue;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> sum{0};
    std::atomic<uint64_t> popped{0};

    std::vector<std::thread> producers;
    for (size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&queue, p]() {
            uint64_t batch[kBatch];
            for (size_t base = 0; base < kPerProducer; base += kBatch) {
                for (size_t i = 0; i < kBatch; ++i) {
                    batch[i] = p * kPerProducer + base + i;
                }
                while (!queue.try_push_batch(batch, kBatch)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::thread> consumers;
    for (size_t c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&]() {
            uint64_t value = 0;
            while (true) {
                if (queue.try_pop(value)) {
                    sum.fetch_add(value, std::memory_order_relaxed);
                    popped.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (done.load(std::memory_order_acquire) &&
                    popped.load(std::memory_order_relaxed) >= kTotal) {
                    break;
                }
                std::this_thread::yield();
            }
        });
    }

    for (std::thread& producer : producers) {
        producer.join();
    }
    done.store(true, std::memory_order_release);
    for (std::thread& consumer : consumers) {
        consumer.join();
    }

    uint64_t expected_sum = 0;
    for (size_t p = 0; p < kProducers; ++p) {
        for (size_t i = 0; i < kPerProducer; ++i) {
            expected_sum += p * kPerProducer + i;
        }
    }
    CHECK_EQ(popped.load(), static_cast<uint64_t>(kTotal));
    CHECK_EQ(sum.load(), expected_sum);
}
