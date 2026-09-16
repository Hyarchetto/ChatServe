// LazyBuffer 用例 — 尾部追加 头部消费 阈值压缩
#include "TestMain.h"

#include "conn/LazyBuffer.h"

TEST(lazy_buffer_appends_and_exposes_contiguous_view) {
    LazyBuffer buf;
    buf.append("abc");
    CHECK_EQ(buf.size(), size_t(3));
    CHECK(!buf.empty());
    CHECK_EQ(std::string(buf.data(), buf.size()), std::string("abc"));
}

TEST(lazy_buffer_consume_advances_head_without_copying) {
    LazyBuffer buf;
    buf.append("abcdef");
    buf.consume(2);
    CHECK_EQ(buf.size(), size_t(4));
    CHECK_EQ(std::string(buf.data(), buf.size()), std::string("cdef"));
}

TEST(lazy_buffer_becomes_empty_after_full_consume) {
    LazyBuffer buf;
    buf.append("abc");
    buf.consume(3);
    CHECK(buf.empty());
    CHECK_EQ(buf.size(), size_t(0));
}

TEST(lazy_buffer_consume_more_than_available_clears) {
    // 消费量超过已有字节时按清零处理 不越界
    LazyBuffer buf;
    buf.append("abc");
    buf.consume(99);
    CHECK(buf.empty());
    CHECK_EQ(buf.size(), size_t(0));
}

TEST(lazy_buffer_compacts_after_head_passes_threshold) {
    // 已消费量过半且越过阈值时物理压缩 剩余内容不变
    LazyBuffer buf;
    buf.append(std::string(8192, 'x'));
    buf.consume(4097);
    CHECK_EQ(buf.size(), size_t(8192 - 4097));
    buf.append("tail");
    CHECK_EQ(std::string(buf.data(), buf.size()),
             std::string(8192 - 4097, 'x') + "tail");
}

TEST(lazy_buffer_empty_on_fresh_instance) {
    LazyBuffer buf;
    CHECK(buf.empty());
    CHECK_EQ(buf.size(), size_t(0));
}
