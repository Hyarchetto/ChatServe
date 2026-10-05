// HttpHandler 用例 — 请求决策与连接关闭意图
#include "TestMain.h"

#include <string>
#include <string_view>

#include "http/HttpHandler.h"

TEST(http_handler_routes_request) {
    HttpHandler h;
    HttpRequestState st;
    HttpAction a = h.handle("GET / HTTP/1.1\r\nHost: x\r\n\r\n", st);
    CHECK_EQ(a.responses_.size(), size_t(1));
    CHECK(!a.close_);
    CHECK(!a.upgrade_);
}

TEST(http_handler_closes_when_client_asks_close) {
    HttpHandler h;
    HttpRequestState st;
    HttpAction a = h.handle("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", st);
    CHECK_EQ(a.responses_.size(), size_t(1));
    CHECK(a.close_);
}

TEST(http_handler_keeps_connection_on_keep_alive) {
    HttpHandler h;
    HttpRequestState st;
    HttpAction a = h.handle("GET / HTTP/1.1\r\nConnection: keep-alive\r\n\r\n", st);
    CHECK(!a.close_);
}

TEST(http_handler_does_not_treat_upgrade_as_close) {
    HttpHandler h;
    HttpRequestState st;
    HttpAction a = h.handle(
        "GET /chat HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", st);
    CHECK(a.upgrade_);
    CHECK(!a.close_);
}

TEST(http_handler_closes_on_bad_request) {
    HttpHandler h;
    HttpRequestState st;
    HttpAction a = h.handle("BADLINE\r\n\r\n", st);
    CHECK(a.close_);
    // 回包在发之前就把关闭意图告知客户端
    CHECK(a.responses_[0].find("connection: close") != std::string::npos);
}

TEST(http_handler_consumes_only_handled_bytes) {
    // 同段到达两个请求，未要求关闭时两个都处理
    HttpHandler h;
    HttpRequestState st;
    HttpAction a = h.handle("GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n", st);
    CHECK_EQ(a.responses_.size(), size_t(2));
    CHECK_EQ(a.consumed_, size_t(36));
    CHECK(!a.close_);
}

TEST(http_handler_stops_at_close_request) {
    // 前一个请求要求关闭，后一个不再处理
    HttpHandler h;
    HttpRequestState st;
    HttpAction a = h.handle("GET / HTTP/1.1\r\nConnection: close\r\n\r\nGET / HTTP/1.1\r\n\r\n", st);
    CHECK_EQ(a.responses_.size(), size_t(1));
    CHECK(a.close_);
}

TEST(http_handler_resumes_upgrade_split_across_calls) {
    // 半截升级请求分两次到达，第二次把未消费的残留与后续字节一并喂
    HttpHandler h;
    HttpRequestState st;
    const std::string req =
        "GET /chat HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    const size_t cut = req.size() / 2;

    HttpAction a = h.handle(std::string_view(req).substr(0, cut), st);
    CHECK(!a.upgrade_);
    CHECK(!a.close_);

    HttpAction b = h.handle(std::string_view(req).substr(a.consumed_), st);
    CHECK(b.upgrade_);
    CHECK(!b.close_);
    CHECK_EQ(a.consumed_ + b.consumed_, req.size());
}
