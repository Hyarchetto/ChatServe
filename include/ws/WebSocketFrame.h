// WebSocket 帧构造 — RFC 6455
// 本文件只做一件事：将应用数据封装为 WebSocket 线路帧
#pragma once

#include <string>

#include "WebSocketOpcode.h"  // WebSocketOpcode

// WebSocket 帧构造器
class WebSocketFrame {
public:
    WebSocketFrame() = delete;

    // 构造一个 WebSocket 帧，服务器发客户端无需掩码
    static std::string build(WebSocketOpcode opcode,
                              const std::string& payload);

    // 对数据应用 WebSocket 掩码 XOR，被 build 和 WebSocketParser 共用
    static void apply_mask(uint8_t* data, size_t len, const uint8_t mask[4]);
};
