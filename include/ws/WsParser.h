// WebSocket 帧解析 — RFC 6455
#pragma once

#include <string>
#include <string_view>
#include <cstdint>

#include "WsOpcode.h"
#include "WsFragmentState.h"

// 单帧解析结果类型
enum class WsResultType {
    INCOMPLETE,     // 缓冲里凑不齐一帧
    CONSUMED,       // 帧已吃完无产出，PONG 与并入分片累积的中间帧都归这里
    MESSAGE,        // 交付一条消息
    PING,           // 收到 PING
    CLOSE,          // 收到 CLOSE
    BAD_FRAME,      // 帧头即判出的协议错误
};

// 单帧解析结果
struct WsResult {
    WsResultType type_ = WsResultType::INCOMPLETE;
    WsOpcode     opcode_ = WsOpcode::TEXT;   // MESSAGE 时区分文本与二进制
    std::string  payload_;                   // MESSAGE PING CLOSE 的载荷
    size_t       consumed_ = 0;              // 本帧消耗的字节数
};

// WebSocket 帧解析器，无状态，每次 handle() 只解析一帧
class WsParser {
public:
    WsParser() = default;

    // 解析缓冲里的第一帧，凑不齐即 INCOMPLETE 且不消耗字节
    // 分片消息的载荷续进 frag，FIN 到齐才以 MESSAGE 交付
    static WsResult handle(std::string_view buf, WsFragmentState& frag);
};
