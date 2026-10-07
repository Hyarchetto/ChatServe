// WsParser 用例 — 单帧解析，分片累积，各类协议错误
#include "TestMain.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "ws/WsFragmentState.h"
#include "ws/WsOpcode.h"
#include "ws/WsParser.h"

// 造一个客户端到服务器的掩码帧，解析器只收客户端帧
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
    WsResult r = WsParser::handle(wire, frag);
    CHECK(r.type_ == WsResultType::MESSAGE);
    CHECK(r.opcode_ == WsOpcode::TEXT);
    CHECK_EQ(r.payload_, std::string("hello"));
    CHECK_EQ(r.consumed_, wire.size());
}

TEST(ws_parser_unmasks_binary_payload) {
    WsFragmentState frag;
    std::string payload(300, '\0');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(i & 0x7F);
    }
    std::string wire = client_frame(WsOpcode::BINARY, payload);
    WsResult r = WsParser::handle(wire, frag);
    CHECK(r.type_ == WsResultType::MESSAGE);
    CHECK(r.opcode_ == WsOpcode::BINARY);
    CHECK_EQ(r.payload_, payload);
    CHECK_EQ(r.consumed_, wire.size());
}

TEST(ws_parser_reads_empty_text_frame) {
    // 空载荷的 TEXT 帧是一条空消息，靠 type_ 与无产出帧区分
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::TEXT, "");
    WsResult r = WsParser::handle(wire, frag);
    CHECK(r.type_ == WsResultType::MESSAGE);
    CHECK(r.payload_.empty());
    CHECK_EQ(r.consumed_, wire.size());
}

TEST(ws_parser_leaves_partial_frame_unconsumed) {
    // 头部齐了但 payload 没到齐，一个字节都不消耗
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::TEXT, "hello");
    WsResult r = WsParser::handle(std::string_view(wire).substr(0, wire.size() - 2), frag);
    CHECK(r.type_ == WsResultType::INCOMPLETE);
    CHECK_EQ(r.consumed_, size_t(0));
}

TEST(ws_parser_leaves_truncated_header_unconsumed) {
    // 连 2 字节帧头都没凑齐
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::TEXT, "hello");
    WsResult r = WsParser::handle(std::string_view(wire).substr(0, 1), frag);
    CHECK(r.type_ == WsResultType::INCOMPLETE);
    CHECK_EQ(r.consumed_, size_t(0));
}

TEST(ws_parser_consumes_one_frame_per_call) {
    // 一段缓冲装两帧，一次调用只吃第一帧
    WsFragmentState frag;
    std::string first = client_frame(WsOpcode::TEXT, "one");
    std::string wire = first + client_frame(WsOpcode::TEXT, "two");
    WsResult r = WsParser::handle(wire, frag);
    CHECK(r.type_ == WsResultType::MESSAGE);
    CHECK_EQ(r.payload_, std::string("one"));
    CHECK_EQ(r.consumed_, first.size());
}

TEST(ws_parser_holds_fragment_start) {
    // 分片起始帧本身不交付消息，载荷寄存进 frag
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::TEXT, "hel", false);
    WsResult r = WsParser::handle(wire, frag);
    CHECK(r.type_ == WsResultType::CONSUMED);
    CHECK_EQ(r.consumed_, wire.size());
    CHECK(frag.in_fragmented_);
    CHECK_EQ(frag.buffer_, std::string("hel"));
}

TEST(ws_parser_joins_fragments_into_one_message) {
    WsFragmentState frag;
    WsResult first = WsParser::handle(client_frame(WsOpcode::TEXT, "hel", false), frag);
    CHECK(first.type_ == WsResultType::CONSUMED);
    WsResult second = WsParser::handle(client_frame(WsOpcode::CONTINUATION, "lo", true), frag);
    CHECK(second.type_ == WsResultType::MESSAGE);
    CHECK(second.opcode_ == WsOpcode::TEXT);
    CHECK_EQ(second.payload_, std::string("hello"));
    CHECK(!frag.in_fragmented_);
}

TEST(ws_parser_keeps_binary_type_across_fragments) {
    // 成消息的类型由起始帧定，continuation 帧的 opcode 不参与
    WsFragmentState frag;
    WsParser::handle(client_frame(WsOpcode::BINARY, "ab", false), frag);
    WsResult r = WsParser::handle(client_frame(WsOpcode::CONTINUATION, "cd", true), frag);
    CHECK(r.type_ == WsResultType::MESSAGE);
    CHECK(r.opcode_ == WsOpcode::BINARY);
    CHECK_EQ(r.payload_, std::string("abcd"));
}

TEST(ws_parser_reads_ping) {
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::PING, "p");
    WsResult r = WsParser::handle(wire, frag);
    CHECK(r.type_ == WsResultType::PING);
    CHECK_EQ(r.payload_, std::string("p"));
    CHECK_EQ(r.consumed_, wire.size());
}

TEST(ws_parser_ignores_pong) {
    // PONG 消费掉即可，没有对上层要交代的东西
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::PONG, "p");
    WsResult r = WsParser::handle(wire, frag);
    CHECK(r.type_ == WsResultType::CONSUMED);
    CHECK_EQ(r.consumed_, wire.size());
}

TEST(ws_parser_reads_close) {
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::CLOSE, "bye");
    WsResult r = WsParser::handle(wire, frag);
    CHECK(r.type_ == WsResultType::CLOSE);
    CHECK_EQ(r.payload_, std::string("bye"));
}

TEST(ws_parser_closes_on_unmasked_frame) {
    // 客户端帧必须掩码 RFC 6455 §5.1
    WsFragmentState frag;
    std::string wire = client_frame(WsOpcode::TEXT, "hi");
    wire[1] = static_cast<char>(static_cast<uint8_t>(wire[1]) & 0x7F);
    WsResult r = WsParser::handle(wire, frag);
    CHECK(r.type_ == WsResultType::BAD_FRAME);
}

TEST(ws_parser_closes_on_oversize_single_frame) {
    // 整帧本身就是一条完整消息，不拆帧同样受消息上限约束
    WsFragmentState frag;
    std::string big(WsFragmentState::kMaxMessageBytes + 1, 'a');
    WsResult r = WsParser::handle(client_frame(WsOpcode::TEXT, big), frag);
    CHECK(r.type_ == WsResultType::BAD_FRAME);
}

TEST(ws_parser_rejects_bad_frame_from_header_alone) {
    // 声明 8MB 载荷的 PING，只给 14 字节帧头，按帧头就该关，载荷一字节不给
    WsFragmentState frag;
    std::string full = client_frame(WsOpcode::PING, std::string(8 * 1024 * 1024, 'x'));
    WsResult r = WsParser::handle(std::string_view(full).substr(0, 14), frag);
    CHECK(r.type_ == WsResultType::BAD_FRAME);
    CHECK_EQ(r.consumed_, size_t(0));   // 判错的帧不消耗字节
}

TEST(ws_parser_closes_on_reserved_opcode) {
    // 保留 opcode 按协议错误关连接，载荷不再回显
    WsFragmentState frag;
    WsResult r = WsParser::handle(client_frame(static_cast<WsOpcode>(0x03), "x"), frag);
    CHECK(r.type_ == WsResultType::BAD_FRAME);
    CHECK(r.payload_.empty());
}

TEST(ws_parser_closes_on_oversize_control_frame) {
    // 控制帧 payload 上限 125 RFC 6455 §5.5
    WsFragmentState frag;
    WsResult r = WsParser::handle(client_frame(WsOpcode::PING, std::string(126, 'x')), frag);
    CHECK(r.type_ == WsResultType::BAD_FRAME);
}

TEST(ws_parser_closes_on_orphan_continuation) {
    // 无分片在途却收到 CONTINUATION
    WsFragmentState frag;
    WsResult r = WsParser::handle(client_frame(WsOpcode::CONTINUATION, "x", true), frag);
    CHECK(r.type_ == WsResultType::BAD_FRAME);
}

TEST(ws_parser_closes_when_data_frame_interrupts_fragments) {
    // 分片进行中又来一个新的数据帧 RFC 6455 §5.4，错误落在第二帧
    WsFragmentState frag;
    WsResult first = WsParser::handle(client_frame(WsOpcode::TEXT, "hel", false), frag);
    CHECK(first.type_ == WsResultType::CONSUMED);
    WsResult second = WsParser::handle(client_frame(WsOpcode::TEXT, "oops", true), frag);
    CHECK(second.type_ == WsResultType::BAD_FRAME);
}

TEST(ws_parser_closes_on_oversize_fragmented_message) {
    // 单帧上限挡不住多帧累积，整条消息另有总量上限，错误落在第二帧
    WsFragmentState frag;
    std::string big(WsFragmentState::kMaxMessageBytes, 'a');
    WsResult first = WsParser::handle(client_frame(WsOpcode::TEXT, big, false), frag);
    CHECK(first.type_ == WsResultType::CONSUMED);
    WsResult second = WsParser::handle(client_frame(WsOpcode::CONTINUATION, "more", true), frag);
    CHECK(second.type_ == WsResultType::BAD_FRAME);
}
