// WebSocket 帧解析 — RFC 6455
#include "ws/WsParser.h"

#include <cstring>

#include "ws/WsFrame.h"

// 单帧上限取自消息上限，整帧本身就是一条完整消息
static constexpr size_t kMaxFramePayloadLen = WsFragmentState::kMaxMessageBytes;
static constexpr size_t kMaxControlPayloadLen = 125;              // RFC 6455 §5.5 控制帧 payload 上限

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

    if (buf.size() < 2) {
        return result;  // INCOMPLETE，2 字节头部都不够
    }

    uint8_t b0 = static_cast<uint8_t>(buf[0]);
    uint8_t b1 = static_cast<uint8_t>(buf[1]);

    bool fin = (b0 & 0x80) != 0;
    uint8_t opcode_val = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    uint64_t payload_len = b1 & 0x7F;

    size_t header_size = 2;

    // 扩展长度
    if (payload_len == 126) {
        if (buf.size() < 4) {
            return result;
        }
        payload_len = (static_cast<uint64_t>(static_cast<uint8_t>(buf[2])) << 8) |
                       static_cast<uint8_t>(buf[3]);
        header_size = 4;
    }
    else if (payload_len == 127) {
        if (buf.size() < 10) {
            return result;
        }
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | static_cast<uint8_t>(buf[2 + i]);
        }
        header_size = 10;
    }

    // 掩码键：本解析器只解析客户端→服务器帧，RFC 6455 §5.1 要求必须掩码
    if (!masked) {
        result.type_ = WsResultType::BAD_FRAME;
        return result;
    }

    // 帧头能判的错误到此判完，判错的帧不消耗字节
    WsOpcode opcode = static_cast<WsOpcode>(opcode_val);
    if (is_rejected_by_header(opcode, fin, payload_len, frag)) {
        result.type_ = WsResultType::BAD_FRAME;
        return result;
    }

    if (buf.size() < header_size + 4) {
        return result;
    }
    uint8_t masking_key[4];
    std::memcpy(masking_key, buf.data() + header_size, 4);
    header_size += 4;

    if (buf.size() < header_size + static_cast<size_t>(payload_len)) {
        return result;
    }

    std::string payload(buf.data() + header_size, payload_len);
    WsFrame::apply_mask(reinterpret_cast<uint8_t*>(payload.data()),
                        payload.size(), masking_key);
    result.consumed_ = header_size + static_cast<size_t>(payload_len);
    // 帧吃到这里就算吃完了，默认无产出，控制帧与成形的消息各自改判
    result.type_ = WsResultType::CONSUMED;

    // 控制帧，FIN 与载荷上限已在帧头阶段判过
    if (opcode == WsOpcode::PING) {
        result.type_ = WsResultType::PING;
        result.payload_ = std::move(payload);
        return result;
    }
    if (opcode == WsOpcode::CLOSE) {
        result.type_ = WsResultType::CLOSE;
        result.payload_ = std::move(payload);
        return result;
    }
    // PONG 没有对上层要交代的东西
    if (opcode == WsOpcode::PONG) {
        return result;
    }

    // 数据帧，分片相关的错误已在帧头阶段判过
    if (opcode == WsOpcode::CONTINUATION) {
        frag.buffer_.append(payload);
        if (!fin) {
            return result;  // 消息还没到齐，载荷已并入 frag
        }
        // 完整消息的类型由起始帧定，continuation 帧的 opcode 不参与
        result.type_ = WsResultType::MESSAGE;
        result.opcode_ = frag.first_opcode_;
        result.payload_ = std::move(frag.buffer_);
        frag.buffer_.clear();
        frag.in_fragmented_ = false;
        return result;
    }
    if (!fin) {
        // 分片开始
        frag.in_fragmented_ = true;
        frag.first_opcode_ = opcode;
        frag.buffer_ = std::move(payload);
        return result;
    }
    result.type_ = WsResultType::MESSAGE;
    result.opcode_ = opcode;
    result.payload_ = std::move(payload);
    return result;
}
