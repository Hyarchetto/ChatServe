// WebSocket Opcode — RFC 6455
#pragma once

#include <cstdint>

enum class WsOpcode : uint8_t {
    CONTINUATION = 0x0,
    TEXT         = 0x1,
    BINARY       = 0x2,
    CLOSE        = 0x8,
    PING         = 0x9,
    PONG         = 0xA,
};
