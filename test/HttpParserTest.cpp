// HttpParser 用例 — 请求行 请求头 请求体 各类解析错误
#include "TestMain.h"

#include <string>

#include "http/HttpParser.h"

// 头部累计上限 与 HttpParser.cpp 中的 kMaxHeaderBytes 一致
static constexpr size_t kHeaderLimit = 64 * 1024;

TEST(http_parser_reads_request_line) {
    std::string req = "GET /chat HTTP/1.1\r\n\r\n";
    HttpResult r = HttpParser::handle(req);
    CHECK(r.type_ == HttpResultType::OK);
    CHECK_EQ(r.request_.method_, std::string("GET"));
    CHECK_EQ(r.request_.path_, std::string("/chat"));
    CHECK_EQ(r.request_.version_, std::string("HTTP/1.1"));
    CHECK_EQ(r.consumed_, req.size());
}

TEST(http_parser_leaves_incomplete_request_pending) {
    HttpResult r = HttpParser::handle("GET /chat HTTP/1.1\r\nHost: x\r\n");
    CHECK(r.type_ == HttpResultType::INCOMPLETE);
    CHECK_EQ(r.consumed_, size_t(0));
}

TEST(http_parser_parses_headers_case_insensitively) {
    HttpResult r = HttpParser::handle("GET / HTTP/1.1\r\ncontent-length: 0\r\nX-A: b\r\n\r\n");
    CHECK(r.type_ == HttpResultType::OK);
    auto v = r.request_.headers_.find("Content-Length");
    CHECK(v != nullptr);
    CHECK_EQ(*v, std::string("0"));
}

TEST(http_parser_trims_whitespace_before_header_colon) {
    // 冒号前的空白是畸形写法 但留着会让头名和查找用的名字对不上 整个头被静默忽略
    // 那样 5 字节 body 不会被消耗 被当成下一个请求的开头
    std::string req = "POST /up HTTP/1.1\r\nContent-Length : 5\r\n\r\nhello";
    HttpResult r = HttpParser::handle(req);
    CHECK(r.type_ == HttpResultType::OK);
    CHECK_EQ(r.request_.body_, std::string("hello"));
    CHECK_EQ(r.consumed_, req.size());
}

TEST(http_parser_merges_duplicate_headers_differing_only_in_case) {
    // 大小写不同的同名字段归到同一个键 后写的值覆盖先写的
    HttpResult r = HttpParser::handle("GET / HTTP/1.1\r\nX-A: 1\r\nX-A: 2\r\n\r\n");
    CHECK(r.type_ == HttpResultType::OK);
    CHECK_EQ(r.request_.headers_.size(), size_t(1));
    auto v = r.request_.headers_.find("x-a");
    CHECK(v != nullptr);
    CHECK_EQ(*v, std::string("2"));
}

TEST(http_parser_rejects_duplicate_content_length) {
    // 两条长度不一致时中间层与服务器可能各取一个 直接判错
    HttpResult r = HttpParser::handle(
        "POST /up HTTP/1.1\r\nContent-Length: 5\r\nCONTENT-LENGTH: 3\r\n\r\nabc");
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
    CHECK_EQ(r.error_msg_, std::string("Duplicate Content-Length"));
}

TEST(http_parser_rejects_transfer_encoding_with_content_length) {
    HttpResult r = HttpParser::handle(
        "POST /up HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc");
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
    CHECK_EQ(r.error_msg_, std::string("Ambiguous message length"));
}

TEST(http_parser_rejects_empty_header_name) {
    HttpResult r = HttpParser::handle("GET / HTTP/1.1\r\n : value\r\n\r\n");
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
}

TEST(http_parser_trims_leading_whitespace_in_header_value) {
    HttpResult r = HttpParser::handle("GET / HTTP/1.1\r\nHost:   example\r\n\r\n");
    auto v = r.request_.headers_.find("Host");
    CHECK(v != nullptr);
    CHECK_EQ(*v, std::string("example"));
}

TEST(http_parser_reads_body_by_content_length) {
    std::string req = "POST /up HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
    HttpResult r = HttpParser::handle(req);
    CHECK(r.type_ == HttpResultType::OK);
    CHECK_EQ(r.request_.body_, std::string("hello"));
    CHECK_EQ(r.consumed_, req.size());
}

TEST(http_parser_leaves_body_incomplete_when_bytes_missing) {
    HttpResult r = HttpParser::handle("POST /up HTTP/1.1\r\nContent-Length: 5\r\n\r\nhel");
    CHECK(r.type_ == HttpResultType::INCOMPLETE);
    CHECK_EQ(r.consumed_, size_t(0));
}

TEST(http_parser_rejects_bad_content_length) {
    HttpResult r = HttpParser::handle("POST /up HTTP/1.1\r\nContent-Length: abc\r\n\r\n");
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
    CHECK_EQ(r.error_msg_, std::string("Invalid Content-Length"));
}

TEST(http_parser_rejects_oversize_body) {
    HttpResult r = HttpParser::handle("POST /up HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n");
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
    CHECK_EQ(r.error_msg_, std::string("Request body too large"));
}

TEST(http_parser_rejects_header_line_without_terminator) {
    // 一直没有 CRLF 且缓冲区越过上限
    std::string req = "GET / HTTP/1.1\r\nX-Big: " + std::string(kHeaderLimit + 1, 'a');
    HttpResult r = HttpParser::handle(req);
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
    CHECK_EQ(r.error_msg_, std::string("Header too large"));
}

TEST(http_parser_rejects_many_short_header_lines) {
    // 每行都很短 但累计消耗越过上限
    std::string req = "GET / HTTP/1.1\r\n";
    while (req.size() < kHeaderLimit + 2) {
        req += "X-Pad: 1\r\n";
    }
    HttpResult r = HttpParser::handle(req);
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
}

TEST(http_parser_does_not_flag_large_body_as_oversize_header) {
    // 头部正常结束 之后的大 body 落在缓冲区里 不能被当成头部超限
    std::string body(200 * 1024, 'x');
    std::string req = "POST /up HTTP/1.1\r\nContent-Length: " + std::to_string(body.size())
                    + "\r\n\r\n" + body;
    HttpResult r = HttpParser::handle(req);
    CHECK(r.type_ == HttpResultType::OK);
    CHECK_EQ(r.request_.body_.size(), body.size());
}

TEST(http_parser_detects_websocket_upgrade) {
    HttpResult r = HttpParser::handle(
        "GET /chat HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    CHECK(r.type_ == HttpResultType::WS_UPGRADE);
}

TEST(http_parser_marks_close_request) {
    HttpResult r = HttpParser::handle("GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
    CHECK(r.type_ == HttpResultType::OK);
    CHECK(r.close_);
}

TEST(http_parser_marks_close_inside_token_list) {
    // Connection 的值是逗号分隔的 token 列表 不能整串比对
    HttpResult r = HttpParser::handle("GET / HTTP/1.1\r\nConnection: keep-alive, close\r\n\r\n");
    CHECK(r.close_);
}

TEST(http_parser_matches_connection_token_case_insensitively) {
    HttpResult r = HttpParser::handle("GET / HTTP/1.1\r\nConnection: CLOSE\r\n\r\n");
    CHECK(r.close_);
}

TEST(http_parser_skips_whitespace_around_token) {
    HttpResult r = HttpParser::handle("GET / HTTP/1.1\r\nConnection:  \tclose \r\n\r\n");
    CHECK(r.close_);
}

TEST(http_parser_does_not_match_close_as_substring) {
    // 按 token 整串比对 子串不算
    HttpResult r = HttpParser::handle("GET / HTTP/1.1\r\nConnection: no-close\r\n\r\n");
    CHECK(!r.close_);
}

TEST(http_parser_keeps_connection_without_close_token) {
    HttpResult r = HttpParser::handle("GET / HTTP/1.1\r\nConnection: keep-alive\r\n\r\n");
    CHECK(!r.close_);
}

TEST(http_parser_requires_upgrade_token_exactly) {
    // 升级判定同样按 token 整串比对 含 upgrade 子串不算
    HttpResult ok = HttpParser::handle(
        "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: keep-alive\r\n\r\n");
    CHECK(ok.type_ == HttpResultType::OK);

    HttpResult bad = HttpParser::handle(
        "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: no-Upgrade\r\n\r\n");
    CHECK(bad.type_ == HttpResultType::OK);
}
