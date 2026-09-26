// BatchQueue 用例 — 攒批 合并唤醒 排空中追加
#include "TestMain.h"

#include <vector>

#include "core/BatchQueue.h"

TEST(batch_queue_hands_whole_batch_to_callback) {
    std::vector<int> seen;
    std::vector<size_t> batch_sizes;
    BatchQueue<int> queue([&](std::vector<int>& items) {
        batch_sizes.push_back(items.size());
        for (int v : items) {
            seen.push_back(v);
        }
    });
    queue.push(1);
    queue.push(2);
    queue.push(3);
    queue.drain();
    // 排空前攒下的三条在同一次交换里取走 回调只被调一次
    CHECK_EQ(batch_sizes.size(), size_t(1));
    CHECK_EQ(batch_sizes[0], size_t(3));
    CHECK_EQ(seen.size(), size_t(3));
    CHECK_EQ(seen[0], 1);
    CHECK_EQ(seen[2], 3);
}

TEST(batch_queue_reports_wake_only_on_empty_to_nonempty) {
    BatchQueue<int> queue([](std::vector<int>&) {});
    // 空到非空 要唤醒
    CHECK(queue.push(1));
    // 排空还没跑 唤醒已在途 再推不必再叫一次
    CHECK(!queue.push(2));
    queue.drain();
    // 排空收尾后回到空 下一次又是空到非空
    CHECK(queue.push(3));
}

TEST(batch_queue_takes_items_pushed_during_drain) {
    std::vector<int> seen;
    BatchQueue<int>* queue = nullptr;
    BatchQueue<int> q([&](std::vector<int>& items) {
        for (int v : items) {
            seen.push_back(v);
        }
        if (seen.size() == 1) {
            // 排空在途时追加的 由同一轮接着取走 不另起一轮
            queue->push(2);
        }
    });
    queue = &q;
    CHECK(q.push(1));
    q.drain();
    CHECK_EQ(seen.size(), size_t(2));
    CHECK_EQ(seen[1], 2);
}

TEST(batch_queue_ignores_empty_batch) {
    BatchQueue<int> queue([](std::vector<int>&) {});
    CHECK(!queue.push_batch(std::vector<int>{}));
}
