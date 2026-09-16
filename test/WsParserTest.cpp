// WsParser 用例 — 帧解析 分片累积 各类协议错误
#include "TestMain.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "ws/WsFrame.h"
#include "ws/WsFragmentState.h"
#include "ws/WsOpcode.h"
#include "ws/WsParser.h"

// 造一个客户端到服务器的掩码帧 解析器只收客户端帧
static std::string client_frame(WsOpcode opcode, std::string_view payload, bool fin = true) {
    static const uint8_t kMask[4] = {0x11, 0x22, 0x33, 0x44};
    std::string f;
    f.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode)));
    if (payload.size() < 126) {
        f.push_back(static_cast<char>(0x80 | payload.size()));
    }
    else if (payload.size() <= 0xFFFF) {
        f.push_back(static_cast<char>(0x80 | 126));
        f.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        f.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    else {
        f.push_back(static_cast<char>(0x80 | 127));
        uint64_t n = payload.size();
        for (int i = 7; i >= 0; --i) {
            f.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
        }
    }
    f.append(reinterpret_cast<const char*>(kMask), 4);
    for (size_t i = 0; i < payload.size(); ++i) {
        f.push_back(static_cast<char>(payload[i] ^ kMask[i % 4]));
    }
    return f;
}

TEST(ws_parser_reads_single_text_frame) {
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::TEXT, "hello");
    WsResult r = WsParser::handle(wire, &frag);
    CHECK(!r.close_);
    CHECK_EQ(r.consumed_, wire.size());
    CHECK_EQ(r.messages_.size(), size_t(1));
    CHECK_EQ(r.messages_[0], std::string("hello"));
}

TEST(ws_parser_unmasks_binary_payload) {
    WsFragmentState frag;
    std::string payload(300, '\0');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(i & 0x7F);
    }
    std::string wire = client_frame(WsOpcode::BINARY, payload);
    WsResult r = WsParser::handle(wire, &frag);
    CHECK_EQ(r.binary_messages_.size(), size_t(1));
    CHECK_EQ(r.binary_messages_[0], payload);
}

TEST(ws_parser_leaves_partial_frame_unconsumed) {
    // 头部齐了但 payload 没到齐 一个字节都不消耗
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::TEXT, "hello");
    WsResult r = WsParser::handle(wire.substr(0, wire.size() - 2), &frag);
    CHECK_EQ(r.consumed_, size_t(0));
    CHECK(r.messages_.empty());
}

TEST(ws_parser_joins_fragments_into_one_message) {
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::TEXT, "hel", false) +
                       client_frame(WsOpcode::CONTINUATION, "lo", true);
    WsResult r = WsParser::handle(wire, &frag);
    CHECK(!r.close_);
    CHECK_EQ(r.messages_.size(), size_t(1));
    CHECK_EQ(r.messages_[0], std::string("hello"));
    CHECK(!frag.in_fragmented_);
}

TEST(ws_parser_reads_multiple_frames_in_one_buffer) {
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::TEXT, "one") +
                       client_frame(WsOpcode::TEXT, "two");
    WsResult r = WsParser::handle(wire, &frag);
    CHECK_EQ(r.messages_.size(), size_t(2));
    CHECK_EQ(r.messages_[0], std::string("one"));
    CHECK_EQ(r.messages_[1], std::string("two"));
    CHECK_EQ(r.consumed_, wire.size());
}

TEST(ws_parser_reads_ping) {
    WsFragmentState frag;
    WsResult r = WsParser::handle(client_frame(WsOpcode::PING, "p"), &frag);
    CHECK(r.ping_);
    CHECK_EQ(r.ping_payload_, std::string("p"));
    CHECK(!r.close_);
}

TEST(ws_parser_closes_on_unmasked_frame) {
    // 客户端帧必须掩码 RFC 6455 §5.1
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::TEXT, "hi");
    wire[1] = static_cast<char>(static_cast<uint8_t>(wire[1]) & 0x7F);
    WsResult r = WsParser::handle(wire, &frag);
    CHECK(r.close_);
}

TEST(ws_parser_closes_on_oversize_control_frame) {
    // 控制帧 payload 上限 125 RFC 6455 §5.5
    WsFragmentState frag;
    WsResult r = WsParser::handle(client_frame(WsOpcode::PING, std::string(126, 'x')), &frag);
    CHECK(r.close_);
}

TEST(ws_parser_closes_on_orphan_continuation) {
    // 无分片在途却收到 CONTINUATION
    WsFragmentState frag;
    WsResult r = WsParser::handle(client_frame(WsOpcode::CONTINUATION, "x", true), &frag);
    CHECK(r.close_);
}

TEST(ws_parser_closes_when_data_frame_interrupts_fragments) {
    // 分片进行中又来一个新的数据帧 RFC 6455 §5.4
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::TEXT, "hel", false) +
                       client_frame(WsOpcode::TEXT, "oops", true);
    WsResult r = WsParser::handle(wire, &frag);
    CHECK(r.close_);
}

TEST(ws_parser_closes_on_oversize_fragmented_message) {
    // 单帧上限挡不住多帧累积 整条消息另有总量上限
    WsFragmentState frag;
    std::string big(WsFragmentState::kMaxMessageBytes, 'a');
    std::string wire = client_frame(WsOpcode::TEXT, big, false) +
                       client_frame(WsOpcode::CONTINUATION, "more", true);
    WsResult r = WsParser::handle(wire, &frag);
    CHECK(r.close_);
}
