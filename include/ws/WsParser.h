// WebSocket 帧解析 — RFC 6455
// 本文件只做一件事：从 TCP buffer 中解析出 WebSocket 帧
#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "WsOpcode.h"
#include "WsFragmentState.h"

// 帧解析结果
struct WsResult {
    size_t consumed_ = 0;                           // 消耗的字节数
    std::vector<std::string> messages_;             // TEXT 帧消息列表
    std::vector<std::string> binary_messages_;      // BINARY 帧消息列表
    bool ping_ = false;
    std::string ping_payload_;
    bool close_ = false;
    std::string close_payload_;
};

// WebSocket 帧解析器，无状态，每次 handle() 从 pos 0 扫描 buffer
class WsParser {
public:
    WsParser() = default;

    // 解析缓冲区，返回解析结果，可能有多条消息
    static WsResult handle(const std::string& buffer,
                                        WsFragmentState* frag);
};
