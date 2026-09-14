// WebSocket 帧构造 — RFC 6455
// 本文件只做一件事：将应用数据封装为 WebSocket 线路帧

#include "ws/WsFrame.h"

// ==================== 帧构造 ====================

std::string WsFrame::build(WsOpcode opcode,
                                   const std::string& payload) {
    return build_from_parts(opcode, {payload});
}

std::string WsFrame::build_from_parts(WsOpcode opcode,
                                      std::initializer_list<std::string_view> parts) {
    size_t payload_len = 0;
    for (auto part : parts) payload_len += part.size();

    uint8_t header[10];
    int header_len = 0;
    // FIN=1, RSV=0, opcode
    header[0] = 0x80 | static_cast<uint8_t>(opcode);
    header_len = 1;
    if (payload_len < 126) {
        header[1] = static_cast<uint8_t>(payload_len);
        header_len = 2;
    } else if (payload_len <= 0xFFFF) {
        header[1] = 126;
        header[2] = (payload_len >> 8) & 0xFF;
        header[3] = payload_len & 0xFF;
        header_len = 4;
    } else {
        header[1] = 127;
        uint64_t elen = payload_len;
        for (int i = 7; i >= 0; --i) {
            header[2 + i] = elen & 0xFF;
            elen >>= 8;
        }
        header_len = 10;
    }

    std::string frame;
    frame.reserve(header_len + payload_len);
    frame.append(reinterpret_cast<char*>(header), header_len);
    for (auto part : parts) frame.append(part.data(), part.size());
    return frame;
}

// WebSocket 掩码异或
void WsFrame::apply_mask(uint8_t* data, size_t len, const uint8_t mask[4]) {
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= mask[i % 4];
    }
}
