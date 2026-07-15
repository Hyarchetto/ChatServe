// WebSocket 帧构造 — RFC 6455
// 本文件只做一件事：将应用数据封装为 WebSocket 线路帧
#include <cstring>

#include "ws/WebSocketFrame.h"

// WebSocket 掩码异或
void WebSocketFrame::apply_mask(uint8_t* data, size_t len, const uint8_t mask[4]) {
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= mask[i % 4];
    }
}

// ==================== 帧构造 ====================

std::string WebSocketFrame::build(WebSocketOpcode opcode,
                                   const std::string& payload) {
    std::string frame;
    uint8_t header[10];
    int header_len = 0;

    // FIN=1, RSV=0, opcode
    header[0] = 0x80 | static_cast<uint8_t>(opcode);
    header_len = 1;

    size_t len = payload.size();
    if (len < 126) {
        header[1] = len;
        header_len = 2;
    } else if (len <= 0xFFFF) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header_len = 4;
    } else {
        header[1] = 127;
        uint64_t elen = len;
        for (int i = 7; i >= 0; --i) {
            header[2 + i] = elen & 0xFF;
            elen >>= 8;
        }
        header_len = 10;
    }

    frame.append(reinterpret_cast<char*>(header), header_len);
    frame.append(payload);
    return frame;
}
