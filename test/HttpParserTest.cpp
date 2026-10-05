// HttpParser 用例 — 请求行、请求头、请求体、跨调用续解析与各类解析错误
#include "TestMain.h"

#include <string>
#include <string_view>

#include "http/HttpParser.h"

namespace {

// 一次性喂完整请求，只验终态
HttpResult parse_all(std::string_view buf) {
    HttpRequestState st;
    return HttpParser::handle(buf, st);
}

// 模拟 ConnHandler 的读缓冲，未消费的半行留在缓冲里与新增字节合并后再喂
struct Feed {
    HttpRequestState st_;
    std::string pending_;

    HttpResult push(std::string_view bytes) {
        this->pending_.append(bytes);
        HttpResult r = HttpParser::handle(this->pending_, this->st_);
        this->pending_.erase(0, r.consumed_);
        return r;
    }
};

}  // namespace

TEST(http_parser_reads_request_line) {
    std::string req = "GET /chat HTTP/1.1\r\n\r\n";
    HttpResult r = parse_all(req);
    CHECK(r.type_ == HttpResultType::OK);
    CHECK_EQ(r.request_.method_, std::string("GET"));
    CHECK_EQ(r.request_.path_, std::string("/chat"));
    CHECK_EQ(r.request_.version_, std::string("HTTP/1.1"));
    CHECK_EQ(r.consumed_, req.size());
}

TEST(http_parser_leaves_incomplete_request_pending) {
    // 完整行随解析消费掉，未终结的那一行留在缓冲里，进度记在 state 上
    Feed f;
    std::string head = "GET /chat HTTP/1.1\r\nHost: x\r\n";
    HttpResult r = f.push(head);
    CHECK(r.type_ == HttpResultType::INCOMPLETE);
    CHECK_EQ(r.consumed_, head.size());
    CHECK_EQ(f.pending_, std::string(""));
    CHECK(f.st_.phase_ == HttpPhase::HEADERS);
    CHECK(f.st_.request_.headers_.find("Host") != nullptr);

    HttpResult done = f.push("\r\n");
    CHECK(done.type_ == HttpResultType::OK);
    CHECK_EQ(done.request_.path_, std::string("/chat"));
}

TEST(http_parser_parses_headers_case_insensitively) {
    HttpResult r = parse_all("GET / HTTP/1.1\r\ncontent-length: 0\r\nX-A: b\r\n\r\n");
    CHECK(r.type_ == HttpResultType::OK);
    auto v = r.request_.headers_.find("Content-Length");
    CHECK(v != nullptr);
    CHECK_EQ(*v, std::string("0"));
}

TEST(http_parser_trims_whitespace_before_header_colon) {
    // 冒号前的空白是畸形写法，但留着会让头名和查找用的名字对不上，整个头被静默忽略
    // 那样 5 字节 body 不会被消耗，被当成下一个请求的开头
    std::string req = "POST /up HTTP/1.1\r\nContent-Length : 5\r\n\r\nhello";
    HttpResult r = parse_all(req);
    CHECK(r.type_ == HttpResultType::OK);
    CHECK_EQ(r.request_.body_, std::string("hello"));
    CHECK_EQ(r.consumed_, req.size());
}

TEST(http_parser_merges_duplicate_headers_differing_only_in_case) {
    // 大小写不同的同名字段归到同一个键，后写的值覆盖先写的
    HttpResult r = parse_all("GET / HTTP/1.1\r\nX-A: 1\r\nx-a: 2\r\n\r\n");
    CHECK(r.type_ == HttpResultType::OK);
    CHECK_EQ(r.request_.headers_.size(), size_t(1));
    auto v = r.request_.headers_.find("x-a");
    CHECK(v != nullptr);
    CHECK_EQ(*v, std::string("2"));
}

TEST(http_parser_rejects_duplicate_content_length) {
    // 两条长度不一致时中间层与服务器可能各取一个，直接判错
    HttpResult r = parse_all(
        "POST /up HTTP/1.1\r\nContent-Length: 5\r\nCONTENT-LENGTH: 3\r\n\r\nabc");
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
    CHECK(r.error_ == HttpError::DUPLICATE_LENGTH);
}

TEST(http_parser_rejects_transfer_encoding) {
    // 头在就拒，块体不会被当成下一条请求
    HttpResult with_cl = parse_all(
        "POST /up HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc");
    CHECK(with_cl.type_ == HttpResultType::BAD_REQUEST);
    CHECK(with_cl.error_ == HttpError::TRANSFER_ENCODING);

    HttpResult chunked = parse_all(
        "POST /up HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    CHECK(chunked.type_ == HttpResultType::BAD_REQUEST);
    CHECK(chunked.error_ == HttpError::TRANSFER_ENCODING);

    // 判据不取值，identity 与其余编码一样拒
    HttpResult identity = parse_all(
        "POST /up HTTP/1.1\r\nTransfer-Encoding: identity\r\nContent-Length: 5\r\n\r\nhello");
    CHECK(identity.type_ == HttpResultType::BAD_REQUEST);
    CHECK(identity.error_ == HttpError::TRANSFER_ENCODING);

    // 没有长度头时块体原本会被留下当下一条请求解析
    HttpResult identity_no_cl = parse_all(
        "POST /up HTTP/1.1\r\nTransfer-Encoding: identity\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    CHECK(identity_no_cl.type_ == HttpResultType::BAD_REQUEST);
    CHECK(identity_no_cl.error_ == HttpError::TRANSFER_ENCODING);
}

TEST(http_parser_rejects_empty_header_name) {
    HttpResult r = parse_all("GET / HTTP/1.1\r\n : value\r\n\r\n");
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
}

TEST(http_parser_trims_whitespace_around_header_value) {
    // 值两侧的 OWS 服务端都要忽略，尾部留着会让按整值比对的头对不上
    HttpResult r = parse_all("GET / HTTP/1.1\r\nHost:  \texample \t\r\n\r\n");
    auto v = r.request_.headers_.find("Host");
    CHECK(v != nullptr);
    CHECK_EQ(*v, std::string("example"));
}

TEST(http_parser_reads_body_by_content_length) {
    std::string req = "POST /up HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
    HttpResult r = parse_all(req);
    CHECK(r.type_ == HttpResultType::OK);
    CHECK_EQ(r.request_.body_, std::string("hello"));
    CHECK_EQ(r.consumed_, req.size());
}

TEST(http_parser_rejects_bad_content_length) {
    HttpResult r = parse_all("POST /up HTTP/1.1\r\nContent-Length: abc\r\n\r\n");
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
    CHECK(r.error_ == HttpError::INVALID_LENGTH);
}

TEST(http_parser_rejects_trailing_garbage_in_content_length) {
    // 整串必须是数字，前缀合法的写法会与中间层取到不同的长度
    HttpResult r = parse_all("POST /up HTTP/1.1\r\nContent-Length: 5x\r\n\r\nhello");
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
    CHECK(r.error_ == HttpError::INVALID_LENGTH);
}

TEST(http_parser_rejects_oversize_body) {
    HttpResult r = parse_all("POST /up HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n");
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
    CHECK(r.error_ == HttpError::BODY_TOO_LARGE);
}

TEST(http_parser_rejects_header_line_without_terminator) {
    // 一直没有 CRLF 且缓冲区越过上限
    std::string req = "GET / HTTP/1.1\r\nX-Big: " + std::string(HttpParser::kMaxHeaderBytes + 1, 'a');
    HttpResult r = parse_all(req);
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
    CHECK(r.error_ == HttpError::HEADER_TOO_LARGE);
}

TEST(http_parser_rejects_many_short_header_lines) {
    // 每行都很短，但累计消耗越过上限
    std::string req = "GET / HTTP/1.1\r\n";
    while (req.size() < HttpParser::kMaxHeaderBytes + 2) {
        req += "X-Pad: 1\r\n";
    }
    HttpResult r = parse_all(req);
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
}

TEST(http_parser_stops_reading_headers_once_the_budget_is_spent) {
    // 一次事件塞进来的头远超上限，每行都完整，读齐的部分越过预算就该停手
    // 停手的判据是 consumed_ 停在预算附近，没有它就得把整个缓冲区解析完才在末尾被拒
    std::string req = "GET / HTTP/1.1\r\n";
    while (req.size() < 200 * 1024) {
        req += "X-Pad: 1\r\n";
    }
    HttpResult r = parse_all(req);
    CHECK(r.type_ == HttpResultType::BAD_REQUEST);
    CHECK(r.error_ == HttpError::HEADER_TOO_LARGE);
    CHECK(r.consumed_ < HttpParser::kMaxHeaderBytes + 1024);
}

TEST(http_parser_does_not_flag_large_body_as_oversize_header) {
    // 头部正常结束，之后的大 body 落在缓冲区里，不能被当成头部超限
    std::string body(200 * 1024, 'x');
    std::string req = "POST /up HTTP/1.1\r\nContent-Length: " + std::to_string(body.size())
                    + "\r\n\r\n" + body;
    HttpResult r = parse_all(req);
    CHECK(r.type_ == HttpResultType::OK);
    CHECK_EQ(r.request_.body_.size(), body.size());
}

TEST(http_parser_detects_websocket_upgrade) {
    HttpResult r = parse_all(
        "GET /chat HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    CHECK(r.type_ == HttpResultType::WS_UPGRADE);
}

TEST(http_parser_detects_upgrade_with_trailing_whitespace) {
    // Upgrade 按整值比对，值尾的 OWS 由解析器先裁掉
    HttpResult r = parse_all(
        "GET /chat HTTP/1.1\r\nUpgrade: websocket \r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    CHECK(r.type_ == HttpResultType::WS_UPGRADE);
}

TEST(http_parser_marks_close_request) {
    HttpResult r = parse_all("GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
    CHECK(r.type_ == HttpResultType::OK);
    CHECK(r.close_);
}

TEST(http_parser_marks_close_inside_token_list) {
    // Connection 的值是逗号分隔的 token 列表，不能整串比对
    HttpResult r = parse_all("GET / HTTP/1.1\r\nConnection: keep-alive, close\r\n\r\n");
    CHECK(r.close_);
}

TEST(http_parser_matches_connection_token_case_insensitively) {
    HttpResult r = parse_all("GET / HTTP/1.1\r\nConnection: CLOSE\r\n\r\n");
    CHECK(r.close_);
}

TEST(http_parser_skips_whitespace_around_token) {
    HttpResult r = parse_all("GET / HTTP/1.1\r\nConnection:  \tclose \r\n\r\n");
    CHECK(r.close_);
}

TEST(http_parser_does_not_match_close_as_substring) {
    // 按 token 整串比对，子串不算
    HttpResult r = parse_all("GET / HTTP/1.1\r\nConnection: no-close\r\n\r\n");
    CHECK(!r.close_);
}

TEST(http_parser_keeps_connection_without_close_token) {
    HttpResult r = parse_all("GET / HTTP/1.1\r\nConnection: keep-alive\r\n\r\n");
    CHECK(!r.close_);
}

TEST(http_parser_requires_upgrade_token_exactly) {
    // 升级判定同样按 token 整串比对，含 upgrade 子串不算
    HttpResult ok = parse_all(
        "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: keep-alive\r\n\r\n");
    CHECK(ok.type_ == HttpResultType::OK);

    HttpResult bad = parse_all(
        "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: no-Upgrade\r\n\r\n");
    CHECK(bad.type_ == HttpResultType::OK);
}

// ======================================== 跨调用续解析 ========================================

TEST(http_parser_resumes_request_line_split_across_calls) {
    Feed f;
    CHECK(f.push("GET /cha").type_ == HttpResultType::INCOMPLETE);
    CHECK_EQ(f.pending_, std::string("GET /cha"));  // 半行没被消费
    CHECK(f.push("t HTTP/1.1\r\n").type_ == HttpResultType::INCOMPLETE);
    CHECK(f.st_.phase_ == HttpPhase::HEADERS);

    HttpResult r = f.push("\r\n");
    CHECK(r.type_ == HttpResultType::OK);
    CHECK_EQ(r.request_.path_, std::string("/chat"));
}

TEST(http_parser_resumes_header_split_mid_line) {
    // 半行留在缓冲里从起点重扫，前缀是完整行的都被消费掉了
    Feed f;
    HttpResult a = f.push("GET / HTTP/1.1\r\nHos");
    CHECK(a.type_ == HttpResultType::INCOMPLETE);
    CHECK_EQ(a.consumed_, size_t(16));
    CHECK_EQ(f.pending_, std::string("Hos"));

    HttpResult b = f.push("t: x\r\n\r\n");
    CHECK(b.type_ == HttpResultType::OK);
    auto v = b.request_.headers_.find("Host");
    CHECK(v != nullptr);
    CHECK_EQ(*v, std::string("x"));
}

TEST(http_parser_resumes_body_in_chunks) {
    Feed f;
    HttpResult a = f.push("POST /up HTTP/1.1\r\nContent-Length: 11\r\n\r\nhel");
    CHECK(a.type_ == HttpResultType::INCOMPLETE);
    CHECK_EQ(f.pending_, std::string(""));

    HttpResult b = f.push("lo wo");
    CHECK(b.type_ == HttpResultType::INCOMPLETE);
    CHECK_EQ(f.st_.request_.body_, std::string("hello wo"));

    HttpResult c = f.push("rld");
    CHECK(c.type_ == HttpResultType::OK);
    CHECK_EQ(c.request_.body_, std::string("hello world"));
}

TEST(http_parser_resumes_pipelined_across_calls) {
    // 第一条完成时 state 复位，第二条的半截留在缓冲里继续喂
    Feed f;
    HttpResult a = f.push("GET / HTTP/1.1\r\n\r\nGET /c");
    CHECK(a.type_ == HttpResultType::OK);
    CHECK_EQ(a.consumed_, size_t(18));
    CHECK_EQ(f.pending_, std::string("GET /c"));

    HttpResult b = f.push("hat HTTP/1.1\r\n\r\n");
    CHECK(b.type_ == HttpResultType::OK);
    CHECK_EQ(b.request_.path_, std::string("/chat"));
}

TEST(http_parser_counts_header_bytes_as_a_running_total) {
    // 读齐一行就计入，任何时候都等于已消费掉的请求行与头部字节数
    Feed f;
    CHECK(f.push("GET / HTTP/1.1\r\nX-A: 1\r\n").type_ == HttpResultType::INCOMPLETE);
    CHECK_EQ(f.st_.header_bytes_, size_t(24));

    CHECK(f.push("X-B: 2\r\n").type_ == HttpResultType::INCOMPLETE);
    CHECK_EQ(f.st_.header_bytes_, size_t(32));

    CHECK(f.push("X-C: 3\r\n\r\n").type_ == HttpResultType::OK);
    CHECK_EQ(f.st_.header_bytes_, size_t(0));
}

TEST(http_parser_accumulates_header_cap_across_calls) {
    // 每行都完整到达，缓冲里始终只有一行，上限只能靠跨调用累计判
    Feed f;
    CHECK(f.push("GET / HTTP/1.1\r\n").type_ == HttpResultType::INCOMPLETE);

    bool rejected = false;
    for (int i = 0; i < 7000; ++i) {
        HttpResult r = f.push("X-Pad: 1\r\n");
        if (r.type_ == HttpResultType::BAD_REQUEST) {
            CHECK(r.error_ == HttpError::HEADER_TOO_LARGE);
            rejected = true;
            break;
        }
        CHECK_EQ(r.consumed_, size_t(10));
        CHECK_EQ(f.pending_, std::string(""));
    }
    CHECK(rejected);
}

TEST(http_parser_caps_unterminated_line_across_calls) {
    // 单行一直无 CRLF，缓冲只增不减，累计越过上限即拒
    Feed f;
    CHECK(f.push("GET / HTTP/1.1\r\n").type_ == HttpResultType::INCOMPLETE);

    bool rejected = false;
    std::string chunk(1000, 'a');
    for (int i = 0; i < 70; ++i) {
        if (f.push(chunk).type_ == HttpResultType::BAD_REQUEST) {
            rejected = true;
            break;
        }
    }
    CHECK(rejected);
}

TEST(http_parser_resets_state_between_pipelined_requests) {
    Feed f;
    HttpResult a = f.push("POST /a HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello");
    CHECK(a.type_ == HttpResultType::OK);
    CHECK_EQ(a.request_.body_, std::string("hello"));

    // 第二条不得继承上一条的长度与体
    HttpResult b = f.push("GET /b HTTP/1.1\r\n\r\n");
    CHECK(b.type_ == HttpResultType::OK);
    CHECK(b.request_.body_.empty());
    CHECK_EQ(b.request_.path_, std::string("/b"));
    CHECK(f.st_.phase_ == HttpPhase::REQUEST_LINE);
}
