// AppParser 用例 — 自定义文本协议的解析与组帧
#include "TestMain.h"

#include "app/AppParser.h"

TEST(app_parser_splits_command_and_params) {
    AppMessage msg = AppParser::parse("MSG|3|hello");
    CHECK_EQ(msg.command_, std::string("MSG"));
    CHECK_EQ(msg.param_count(), size_t(2));
    CHECK_EQ(msg.param(0), std::string("3"));
    CHECK_EQ(msg.param(1), std::string("hello"));
    CHECK_EQ(msg.rest_, std::string("3|hello"));
}

TEST(app_parser_splits_every_delimiter_into_params) {
    // 每个 | 都切参数 内容本身含 | 时要从 rest_ 取回
    AppMessage msg = AppParser::parse("MSG|3|a|b|c");
    CHECK_EQ(msg.param_count(), size_t(4));
    CHECK_EQ(msg.param(0), std::string("3"));
    CHECK_EQ(msg.param(3), std::string("c"));
    CHECK_EQ(msg.rest_, std::string("3|a|b|c"));
}

TEST(app_parser_leaves_bare_text_as_empty_command) {
    // 无 | 的裸文本命令字为空 由上层表查找未命中丢弃
    AppMessage msg = AppParser::parse("hello world");
    CHECK(msg.command_.empty());
    CHECK_EQ(msg.param_count(), size_t(0));
}

TEST(app_parser_handles_empty_param_between_delimiters) {
    AppMessage msg = AppParser::parse("A||B");
    CHECK_EQ(msg.param_count(), size_t(2));
    CHECK(msg.param(0).empty());
    CHECK_EQ(msg.param(1), std::string("B"));
}

TEST(app_parser_builds_frame_from_params) {
    CHECK_EQ(AppParser::build_frame("MSG", "3", "hi"), std::string("MSG|3|hi"));
    CHECK_EQ(AppParser::build_frame("PING"), std::string("PING"));
}

TEST(app_parser_round_trips_through_build) {
    std::string wire = AppParser::build_frame("OFFER", "7", "sdp-payload");
    AppMessage msg = AppParser::parse(wire);
    CHECK_EQ(msg.command_, std::string("OFFER"));
    CHECK_EQ(msg.param(0), std::string("7"));
    CHECK_EQ(msg.param(1), std::string("sdp-payload"));
}
