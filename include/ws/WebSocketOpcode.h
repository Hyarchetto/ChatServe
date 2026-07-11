// WebSocket Opcode — RFC 6455
// 帧构造和帧解析都需要这个枚举，所以单独放一个文件
// 这样构造器不用为了拿枚举而依赖解析器
#pragma once

#include <cstdint>

enum class WebSocketOpcode : uint8_t {
    CONTINUATION = 0x0,
    TEXT         = 0x1,
    BINARY       = 0x2,
    CLOSE        = 0x8,
    PING         = 0x9,
    PONG         = 0xA,
};
