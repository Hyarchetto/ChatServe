// WsHandler 用例 — 一批缓冲里多帧的汇总、控制帧回执与停机边界
#include "TestMain.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "ws/WsFragmentState.h"
#include "ws/WsHandler.h"
#include "ws/WsOpcode.h"

// 造一个客户端到服务器的掩码帧
static std::string client_frame(WsOpcode opcode, std::string_view payload, bool fin = true) {
    static const uint8_t kMask[4] = {0x55, 0x66, 0x77, 0x88};
    std::string f;
    f.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode)));
    f.push_back(static_cast<char>(0x80 | payload.size()));
    f.append(reinterpret_cast<const char*>(kMask), 4);
    for (size_t i = 0; i < payload.size(); ++i) {
        f.push_back(static_cast<char>(payload[i] ^ kMask[i % 4]));
    }
    return f;
}

// 服务器发出的帧不带掩码，载荷都短于一字节长度上限
static std::string server_frame(WsOpcode opcode, std::string_view payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));
    f.push_back(static_cast<char>(payload.size()));
    f.append(payload);
    return f;
}

TEST(ws_handler_consumes_nothing_on_empty_buffer) {
    WsFragmentState frag;
    WsHandler h;
    WsAction a = h.handle("", frag);
    CHECK_EQ(a.consumed_, size_t(0));
    CHECK(a.messages_.empty());
}

TEST(ws_handler_joins_two_frames_in_one_buffer) {
    WsFragmentState frag;
    WsHandler h;
    std::string wire = client_frame(WsOpcode::TEXT, "one") +
                       client_frame(WsOpcode::TEXT, "two");
    WsAction a = h.handle(wire, frag);
    CHECK_EQ(a.messages_.size(), size_t(2));
    CHECK_EQ(a.messages_[0], std::string("one"));
    CHECK_EQ(a.messages_[1], std::string("two"));
    CHECK_EQ(a.consumed_, wire.size());
    CHECK(!a.close_);
}

TEST(ws_handler_routes_binary_to_binaries) {
    WsFragmentState frag;
    WsHandler h;
    std::string wire = client_frame(WsOpcode::BINARY, "chunk");
    WsAction a = h.handle(wire, frag);
    CHECK(a.messages_.empty());
    CHECK_EQ(a.binaries_.size(), size_t(1));
    CHECK_EQ(a.binaries_[0], std::string("chunk"));
}

TEST(ws_handler_replies_to_every_ping) {
    // 一批里的每条 PING 各回一条 PONG，回执顺序与帧序一致
    WsFragmentState frag;
    WsHandler h;
    std::string wire = client_frame(WsOpcode::PING, "a") + client_frame(WsOpcode::PING, "b") +
                       client_frame(WsOpcode::PING, "c") + client_frame(WsOpcode::PING, "d");
    WsAction a = h.handle(wire, frag);
    CHECK_EQ(a.responses_.size(), size_t(4));
    CHECK_EQ(a.responses_[0], server_frame(WsOpcode::PONG, "a"));
    CHECK_EQ(a.responses_[3], server_frame(WsOpcode::PONG, "d"));
    CHECK_EQ(a.consumed_, wire.size());
}

TEST(ws_handler_ignores_pong) {
    WsFragmentState frag;
    WsHandler h;
    std::string wire = client_frame(WsOpcode::PONG, "p");
    WsAction a = h.handle(wire, frag);
    CHECK(a.responses_.empty());
    CHECK(a.messages_.empty());
    CHECK(!a.close_);
    CHECK_EQ(a.consumed_, wire.size());
}

TEST(ws_handler_stops_at_partial_frame) {
    // 尾巴那半帧不消费，留给下次读事件补上
    WsFragmentState frag;
    WsHandler h;
    std::string first = client_frame(WsOpcode::TEXT, "one");
    std::string second = client_frame(WsOpcode::TEXT, "two");
    std::string wire = first + second.substr(0, 3);
    WsAction a = h.handle(wire, frag);
    CHECK_EQ(a.messages_.size(), size_t(1));
    CHECK_EQ(a.messages_[0], std::string("one"));
    CHECK_EQ(a.consumed_, first.size());

    // 施加侧 consume 掉第一帧，补齐后再喂整帧，半帧的进度由调用方保管
    WsAction b = h.handle(second, frag);
    CHECK_EQ(b.messages_.size(), size_t(1));
    CHECK_EQ(b.messages_[0], std::string("two"));
}

TEST(ws_handler_joins_fragments_into_one_message) {
    WsFragmentState frag;
    WsHandler h;
    std::string wire = client_frame(WsOpcode::TEXT, "hel", false) +
                       client_frame(WsOpcode::CONTINUATION, "lo", true);
    WsAction a = h.handle(wire, frag);
    CHECK_EQ(a.messages_.size(), size_t(1));
    CHECK_EQ(a.messages_[0], std::string("hello"));
    CHECK_EQ(a.consumed_, wire.size());
    CHECK(!a.close_);
}

TEST(ws_handler_replies_close_and_marks_close) {
    WsFragmentState frag;
    WsHandler h;
    std::string wire = client_frame(WsOpcode::CLOSE, "bye");
    WsAction a = h.handle(wire, frag);
    CHECK(a.close_);
    CHECK_EQ(a.responses_.size(), size_t(1));
    CHECK_EQ(a.responses_[0], server_frame(WsOpcode::CLOSE, "bye"));
}

TEST(ws_handler_delivers_message_before_bad_frame) {
    // 坏帧跟在一好帧后面，好帧的消息照旧交付，随后断开
    WsFragmentState frag;
    WsHandler h;
    std::string bad = client_frame(WsOpcode::TEXT, "x");
    bad[1] = static_cast<char>(static_cast<uint8_t>(bad[1]) & 0x7F);   // 去掉掩码位
    std::string wire = client_frame(WsOpcode::TEXT, "ok") + bad;
    WsAction a = h.handle(wire, frag);
    CHECK_EQ(a.messages_.size(), size_t(1));
    CHECK_EQ(a.messages_[0], std::string("ok"));
    CHECK(a.close_);
    CHECK_EQ(a.responses_.size(), size_t(1));
    CHECK_EQ(a.responses_[0], server_frame(WsOpcode::CLOSE, ""));
}
