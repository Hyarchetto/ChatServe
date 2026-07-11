// WebSocket 帧解析 — RFC 6455
// 本文件只做一件事：从 TCP buffer 中解析出 WebSocket 帧
#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "WebSocketOpcode.h"

// WebSocket 分片累积状态 — RFC 6455 §5.4
//
// 为什么需要这个状态：
//   WebSocket 消息可以分片发送（FIN=0 起始帧 → 若干 CONTINUATION 帧 → FIN=1 结束帧），
//   一个逻辑消息可能分散在多次 TCP read 中到达，必须跨 handle() 调用持续累积，
//   直到收到 FIN=1 才能拼出完整消息。
//
// 相比之下 HTTP 不需要状态，因为 HTTP 请求是自包含的（请求行+头部+正文一次性发完），
//   HttpParser::handle() 每次从头解析，一次调用解决。
//
struct WebSocketFragmentState {
    bool in_fragmented_ = false;                         // true 表示正在收分片消息
    WebSocketOpcode first_opcode_ = WebSocketOpcode::TEXT;  // 起始帧 opcode
    std::string buffer_;                                 // 分片累积缓冲区
};

// 帧解析结果
struct WebSocketParseResult {
    size_t consumed_ = 0;                   // 消耗的字节数
    std::vector<std::string> messages_;     // TEXT 帧消息列表
    std::vector<std::string> binary_messages_;  // BINARY 帧消息列表
    bool ping_ = false;
    std::string ping_payload_;
    bool close_ = false;
    std::string close_payload_;
};

// WebSocket 帧解析器（无状态：每次 handle() 从 pos 0 扫描 buffer）
class WebSocketParser {
public:
    WebSocketParser() = default;

    // 解析缓冲区，返回解析结果（可能有多条消息）
    static WebSocketParseResult handle(const std::string& buffer,
                                        WebSocketFragmentState* frag);
};
