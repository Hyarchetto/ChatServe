// WebSocket 帧解析 — RFC 6455
#include <cstring>
#include <vector>

#include "ws/WsParser.h"
#include "ws/WsFrame.h"

// 单帧上限取自消息上限，整帧本身就是一条完整消息
static constexpr size_t kMaxFramePayloadLen = WsFragmentState::kMaxMessageBytes;
static constexpr size_t kMaxControlPayloadLen = 125;              // RFC 6455 §5.5 控制帧 payload 上限

// 按起始 opcode 投递完整消息到对应列表
static void deliver_message(WsOpcode opcode, std::string message, WsResult& result) {
    if (opcode == WsOpcode::BINARY) {
        result.binary_messages_.push_back(std::move(message));
    }
    else {
        result.messages_.push_back(std::move(message));
    }
}

// 只依赖帧头的协议错误，命中即该关连接
// 判据只看 opcode、FIN、声明长度与已累积的分片状态，不看载荷
static bool is_rejected_by_header(WsOpcode opcode, bool fin, uint64_t payload_len,
                                  const WsFragmentState& frag) {
    // 声明长度先判，后面的累加才不会有回绕
    if (payload_len > kMaxFramePayloadLen) {
        return true;
    }
    if (opcode == WsOpcode::PING || opcode == WsOpcode::PONG || opcode == WsOpcode::CLOSE) {
        // RFC 6455 §5.5 要求控制帧 FIN 必须为 1 且 payload 不超过 125 字节
        return !fin || payload_len > kMaxControlPayloadLen;
    }
    uint8_t opcode_val = static_cast<uint8_t>(opcode);
    // 保留 opcode，RFC 6455 要求关闭连接
    if ((opcode_val >= 0x03 && opcode_val <= 0x07) ||
        (opcode_val >= 0x0B && opcode_val <= 0x0F)) {
        return true;
    }
    if (opcode == WsOpcode::CONTINUATION) {
        // 孤儿 continuation：无分片在途，RFC 6455 §5.4 要求关闭连接
        if (!frag.in_fragmented_) {
            return true;
        }
        // 单帧上限挡不住拆成很多帧累积，同一条消息也要有总量上限
        return frag.buffer_.size() + static_cast<size_t>(payload_len) > WsFragmentState::kMaxMessageBytes;
    }
    // 剩下的是 TEXT/BINARY 数据帧，分片进行中又收到数据帧是协议错误
    return frag.in_fragmented_;
}

// ==================== 帧解析 ====================

// 帧头一解析完就把只依赖帧头的协议错误判完，再等掩码键与载荷
// 非法帧在载荷到齐之前就被拒，省下收齐、分配与解掩码的代价
WsResult WsParser::handle(std::string_view buf, WsFragmentState& frag) {
    WsResult result;

    size_t pos = 0;
    while (pos < buf.size()) {
        if (buf.size() - pos < 2) break;  // 至少需要 2 字节头部

        uint8_t b0 = static_cast<uint8_t>(buf[pos]);
        uint8_t b1 = static_cast<uint8_t>(buf[pos + 1]);

        bool fin = (b0 & 0x80) != 0;
        uint8_t opcode_val = b0 & 0x0F;
        bool masked = (b1 & 0x80) != 0;
        uint64_t payload_len = b1 & 0x7F;

        size_t header_size = 2;

        // 扩展长度
        if (payload_len == 126) {
            if (buf.size() - pos < 4) break;
            payload_len = (static_cast<uint64_t>(static_cast<uint8_t>(buf[pos + 2])) << 8) |
                                                 static_cast<uint8_t>(buf[pos + 3]);
            header_size = 4;
        }
        else if (payload_len == 127) {
            if (buf.size() - pos < 10) break;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) |
                    static_cast<uint8_t>(buf[pos + 2 + i]);
            }
            header_size = 10;
        }

        // 掩码键：本解析器只解析客户端→服务器帧，RFC 6455 §5.1 要求必须掩码
        if (!masked) {
            result.close_ = true;
            break;
        }

        // 帧头能判的错误到此判完，判错的帧不推进 pos 也不等载荷
        WsOpcode opcode = static_cast<WsOpcode>(opcode_val);
        if (is_rejected_by_header(opcode, fin, payload_len, frag)) {
            result.close_ = true;
            break;
        }

        uint8_t masking_key[4];
        if (buf.size() - pos < header_size + 4) break;
        std::memcpy(masking_key, buf.data() + pos + header_size, 4);
        header_size += 4;

        // 检查数据是否完整
        if (buf.size() - pos < header_size + static_cast<size_t>(payload_len)) break;

        // 提取 payload
        std::string payload(buf.data() + pos + header_size, payload_len);
        WsFrame::apply_mask(reinterpret_cast<uint8_t*>(payload.data()),
                   payload.size(), masking_key);

        pos += header_size + payload_len;

        // 控制帧，FIN 与载荷上限已在帧头阶段判过
        if (opcode == WsOpcode::PING || opcode == WsOpcode::PONG ||
            opcode == WsOpcode::CLOSE) {
            if (opcode == WsOpcode::PING) {
                result.ping_ = true;
                result.ping_payload_ = std::move(payload);
            }
            else if (opcode == WsOpcode::CLOSE) {
                result.close_ = true;
                result.close_payload_ = std::move(payload);
            }
            continue;  // PONG 忽略
        }

        // 数据帧，分片相关的错误已在帧头阶段判过
        if (opcode == WsOpcode::CONTINUATION) {
            frag.buffer_.append(payload);
            if (fin) {
                deliver_message(frag.first_opcode_, std::move(frag.buffer_), result);
                frag.buffer_.clear();
                frag.in_fragmented_ = false;
            }
        }
        else if (fin) {
            deliver_message(opcode, std::move(payload), result);
        }
        else {
            // 分片开始
            frag.in_fragmented_ = true;
            frag.first_opcode_ = opcode;
            frag.buffer_ = std::move(payload);
        }
    }

    result.consumed_ = pos;
    return result;
}
