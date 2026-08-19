// WebSocket 帧解析 — RFC 6455
// 本文件只做一件事：从 TCP buffer 中解析出 WebSocket 帧
#include <cstring>
#include <vector>

#include "ws/WsParser.h"
#include "ws/WsFrame.h"

// 帧大小限制
static constexpr size_t kMaxFramePayloadLen = 64 * 1024 * 1024;   // 单帧 payload 上限 64MB
static constexpr size_t kMaxControlPayloadLen = 125;              // RFC 6455 §5.5 控制帧 payload 上限

// 按起始 opcode 投递完整消息到对应列表
static void deliver_message(WsResult& result, WsOpcode opcode, std::string message) {
    if (opcode == WsOpcode::BINARY) {
        result.binary_messages_.push_back(std::move(message));
    }
    else {
        result.messages_.push_back(std::move(message));
    }
}

// ==================== 帧解析 ====================

WsResult WsParser::handle(const std::string& buffer,
                                              WsFragmentState* frag) {
    WsResult result;

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
        } 
        else if (payload_len == 127) {
            if (buffer.size() - pos < 10) break;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) |
                    static_cast<uint8_t>(buffer[pos + 2 + i]);
            }
            header_size = 10;
        }

        // 掩码键：本解析器只解析客户端→服务器帧，RFC 6455 §5.1 要求必须掩码
        if (!masked) {
            result.close_ = true;
            break;
        }
        uint8_t masking_key[4];
        if (buffer.size() - pos < header_size + 4) break;
        std::memcpy(masking_key, buffer.data() + pos + header_size, 4);
        header_size += 4;

        // 单帧 payload 超限直接关闭连接：不推进 pos 也不跳过
        // 若按 header_size + payload_len 推进，payload_len 接近 2^64 时加法会回绕成 0，造成无限循环
        if (payload_len > kMaxFramePayloadLen) {
            result.close_ = true;
            break;
        }

        // 检查数据是否完整
        if (buffer.size() - pos < header_size + static_cast<size_t>(payload_len)) break;

        // 提取 payload
        std::string payload(buffer.data() + pos + header_size, payload_len);
        WsFrame::apply_mask(reinterpret_cast<uint8_t*>(payload.data()),
                   payload.size(), masking_key);

        pos += header_size + payload_len;

        // 处理 opcode
        WsOpcode opcode = static_cast<WsOpcode>(opcode_val);

        // 控制帧：RFC 6455 §5.5 要求 FIN 必须为 1 且 payload ≤ 125 字节
        if (opcode == WsOpcode::PING || opcode == WsOpcode::PONG ||
            opcode == WsOpcode::CLOSE) {
            if (!fin || payload.size() > kMaxControlPayloadLen) {
                result.close_ = true;
                break;
            }
            if (opcode == WsOpcode::PING) {
                result.ping_ = true;
                result.ping_payload_ = payload;
            }
            else if (opcode == WsOpcode::CLOSE) {
                result.close_ = true;
                result.close_payload_ = payload;
            }
            continue;  // PONG 忽略
        }

        // 保留 opcode，RFC 6455 要求关闭连接
        if ((opcode_val >= 0x03 && opcode_val <= 0x07) ||
            (opcode_val >= 0x0B && opcode_val <= 0x0F)) {
            result.close_ = true;
            result.close_payload_ = payload;
            continue;
        }

        // 数据帧
        if (opcode == WsOpcode::CONTINUATION) {
            // 孤儿 continuation：无分片在途，RFC 6455 §5.4 要求关闭连接
            if (!(frag && frag->in_fragmented_)) {
                result.close_ = true;
                break;
            }
            frag->buffer_.append(payload);
            if (fin) {
                deliver_message(result, frag->first_opcode_, std::move(frag->buffer_));
                frag->buffer_.clear();
                frag->in_fragmented_ = false;
            }
        }
        else {
            // TEXT/BINARY 数据帧
            // 分片进行中收到新的数据帧：RFC 6455 §5.4 协议错误，关闭连接
            if (frag && frag->in_fragmented_) {
                result.close_ = true;
                break;
            }
            if (fin) {
                deliver_message(result, opcode, std::move(payload));
            }
            else if (frag) {
                // 分片开始
                frag->in_fragmented_ = true;
                frag->first_opcode_ = opcode;
                frag->buffer_ = std::move(payload);
            }
        }
    }

    result.consumed_ = pos;
    return result;
}
