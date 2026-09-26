// Heartbeat 用例 — 心跳判据的边界
#include "TestMain.h"

#include <chrono>

#include "conn/Heartbeat.h"

using std::chrono::milliseconds;

static milliseconds ms(long v) {
    return milliseconds(v);
}

TEST(heartbeat_keeps_fresh_connection_quiet) {
    CHECK(Heartbeat::judge(ms(0), true) == Heartbeat::Action::NONE);
    CHECK(Heartbeat::judge(ms(1000), false) == Heartbeat::Action::NONE);
}

TEST(heartbeat_pings_idle_websocket) {
    // 阈值与节拍同值时取 >= 到点那次就该发出去 取 > 会整整推迟一个节拍
    CHECK(Heartbeat::judge(ms(30 * 1000), true) == Heartbeat::Action::PING);
    CHECK(Heartbeat::judge(ms(60 * 1000), true) == Heartbeat::Action::PING);
}

TEST(heartbeat_skips_unupgraded_connection) {
    // 未升级的连接没有帧层 发 PING 会被对端当畸形 HTTP 请求 只能等硬超时
    CHECK(Heartbeat::judge(ms(60 * 1000), false) == Heartbeat::Action::NONE);
}

TEST(heartbeat_closes_past_idle_timeout) {
    CHECK(Heartbeat::judge(ms(90 * 1000), true) == Heartbeat::Action::CLOSE);
    CHECK(Heartbeat::judge(ms(90 * 1000), false) == Heartbeat::Action::CLOSE);
    CHECK(Heartbeat::judge(ms(600 * 1000), true) == Heartbeat::Action::CLOSE);
}
