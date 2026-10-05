// WebSocket 帧构造 — RFC 6455
#pragma once

#include <string>
#include <string_view>
#include <initializer_list>

#include "WsOpcode.h"

// WebSocket 帧构造器
class WsFrame {
public:
    WsFrame() = delete;

    // 构造一个 WebSocket 帧，服务器发客户端无需掩码
    static std::string build(WsOpcode opcode, std::string_view payload);

    // 一次构建多段载荷的帧，避免逐段拼接产生临时串
    static std::string build_from_parts(WsOpcode opcode,
                                        std::initializer_list<std::string_view> parts);

    // 对数据应用 WebSocket 掩码 XOR
    static void apply_mask(uint8_t* data, size_t len, const uint8_t mask[4]);
};
