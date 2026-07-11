// WebSocket 帧构造 — RFC 6455
// 本文件只做一件事：将应用数据封装为 WebSocket 线路帧
#pragma once

#include <string>

#include "WebSocketOpcode.h"  // WebSocketOpcode

// WebSocket 帧构造器
class WebSocketFrame {
public:
    WebSocketFrame() = delete;

    // 构造一个 WebSocket 帧（掩码由调用方控制，服务器发客户端无掩码）
    static std::string build(WebSocketOpcode opcode,
                              const std::string& payload,
                              bool mask = false);
};
