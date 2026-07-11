// WebSocket 帧解析 — RFC 6455
// 本文件只做一件事：从 TCP buffer 中解析出 WebSocket 帧
#include <cstring>
#include <vector>

#include "ws/WebSocketParser.h"

// 掩码异或
static void apply_mask(uint8_t* data, size_t len, const uint8_t mask[4]) {
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= mask[i % 4];
    }
}

// ==================== 帧解析 ====================

WebSocketParseResult WebSocketParser::handle(const std::string& buffer,
                                              WebSocketFragmentState* frag) {
    WebSocketParseResult result;

    size_t pos = 0;
    while (pos < buffer.size()) {
        if (buffer.size() - pos < 2) break;  // 至少需要 2 字节头部

        uint8_t b0 = static_cast<uint8_t>(buffer[pos]);
        uint8_t b1 = static_cast<uint8_t>(buffer[pos + 1]);

        bool fin = (b0 & 0x80) != 0;
        uint8_t opcode_val = b0 & 0x0F;
        bool masked = (b1 & 0x80) != 0;
        uint64_t payload_len = b1 & 0x7F;

        size_t header_size = 2;

        // 扩展长度
        if (payload_len == 126) {
            if (buffer.size() - pos < 4) break;
            payload_len = (static_cast<uint64_t>(
                static_cast<uint8_t>(buffer[pos + 2])) << 8) |
                static_cast<uint8_t>(buffer[pos + 3]);
            header_size = 4;
        } else if (payload_len == 127) {
            if (buffer.size() - pos < 10) break;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) |
                    static_cast<uint8_t>(buffer[pos + 2 + i]);
            }
            header_size = 10;
        }

        // 掩码键
        uint8_t masking_key[4];
        if (masked) {
            if (buffer.size() - pos < header_size + 4) break;
            std::memcpy(masking_key, buffer.data() + pos + header_size, 4);
            header_size += 4;
        }

        // 检查数据是否完整
        if (buffer.size() - pos < header_size + payload_len) break;

        // 提取 payload
        std::string payload(buffer.data() + pos + header_size, payload_len);
        if (masked) {
            apply_mask(reinterpret_cast<uint8_t*>(payload.data()),
                       payload.size(), masking_key);
        }

        pos += header_size + payload_len;

        // 处理 opcode
        WebSocketOpcode opcode = static_cast<WebSocketOpcode>(opcode_val);

        // 控制帧
        if (opcode == WebSocketOpcode::PING) {
            result.ping_ = true;
            result.ping_payload_ = payload;
            continue;
        }
        if (opcode == WebSocketOpcode::PONG) {
            continue;  // 忽略 PONG（服务端不发 PING，不会收到 PONG）
        }
        if (opcode == WebSocketOpcode::CLOSE) {
            result.close_ = true;
            result.close_payload_ = payload;
            continue;
        }

        // 数据帧
        if (opcode == WebSocketOpcode::CONTINUATION) {
            if (frag && frag->in_fragmented_) {
                frag->buffer_.append(payload);
                if (fin) {
                    if (frag->first_opcode_ == WebSocketOpcode::BINARY) {
                        result.binary_messages_.push_back(std::move(frag->buffer_));
                    } else {
                        result.messages_.push_back(std::move(frag->buffer_));
                    }
                    frag->buffer_.clear();
                    frag->in_fragmented_ = false;
                }
            }
            // 非法 continuation（无起始帧），丢弃
        } else if (opcode == WebSocketOpcode::BINARY) {
            if (fin) {
                // 完整 binary 消息
                result.binary_messages_.push_back(std::move(payload));
            } else {
                // binary 分片开始
                if (frag) {
                    frag->in_fragmented_ = true;
                    frag->first_opcode_ = opcode;
                    frag->buffer_ = std::move(payload);
                }
            }
        } else {
            // TEXT
            if (fin) {
                // 完整消息
                result.messages_.push_back(std::move(payload));
            } else {
                // 分片开始
                if (frag) {
                    frag->in_fragmented_ = true;
                    frag->first_opcode_ = opcode;
                    frag->buffer_ = std::move(payload);
                }
            }
        }
    }

    result.consumed_ = pos;
    return result;
}
