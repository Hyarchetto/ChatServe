// Heartbeat — 连接静默判死的节拍与阈值，纯函数
#pragma once

#include <chrono>

struct Heartbeat {
    static constexpr std::chrono::milliseconds kTickInterval{30 * 1000};  // 扫描节拍，即定时器周期
    static constexpr std::chrono::milliseconds kIdleTimeout{90 * 1000};   // 静默多久判死

    // 客户端每三十秒发一条应用层 PING，入站时间戳据此刷新
    // 起搏断掉就没人再刷新它，静默满阈值即判死
    static bool is_expired(std::chrono::milliseconds idle) {
        return idle >= kIdleTimeout;
    }
};
