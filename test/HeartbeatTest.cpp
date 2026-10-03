// Heartbeat 用例 — 静默判死的边界
#include "TestMain.h"

#include <chrono>

#include "conn/Heartbeat.h"

using std::chrono::milliseconds;

static milliseconds ms(long v) {
    return milliseconds(v);
}

TEST(heartbeat_keeps_quiet_under_timeout) {
    CHECK(!Heartbeat::is_expired(ms(0)));
    CHECK(!Heartbeat::is_expired(ms(1)));
    CHECK(!Heartbeat::is_expired(ms(89 * 1000)));
    // 阈值到点前最后一毫秒仍算活着，判据取的是 >=
    CHECK(!Heartbeat::is_expired(ms(90 * 1000 - 1)));
}

TEST(heartbeat_expires_at_timeout) {
    CHECK(Heartbeat::is_expired(ms(90 * 1000)));
    CHECK(Heartbeat::is_expired(ms(600 * 1000)));
}
