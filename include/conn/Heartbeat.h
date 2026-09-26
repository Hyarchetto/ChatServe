// Heartbeat — 心跳策略 纯函数 无依赖
#pragma once

#include <chrono>

struct Heartbeat {
    static constexpr std::chrono::milliseconds kTickInterval{30 * 1000};  // 扫描节拍 即定时器周期
    static constexpr std::chrono::milliseconds kPingIdle{30 * 1000};      // 入站静默多久发一条 PING 试探
    static constexpr std::chrono::milliseconds kIdleTimeout{90 * 1000};   // 出入站都静默多久判死

    enum class Action {
        NONE,
        PING,
        CLOSE,
    };

    // 通过活动时间和连接模式返回对应策略
    static Action judge(std::chrono::milliseconds idle, bool ws_mode) {
        if (idle >= kIdleTimeout) {
            return Action::CLOSE;
        }
        // HTTP/1.1没有主动响应功能，干等直到超时
        if (ws_mode && idle >= kPingIdle) {
            return Action::PING;
        }
        return Action::NONE;
    }
};
