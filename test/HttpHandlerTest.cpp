// HttpHandler 用例 — 请求决策与连接关闭意图
#include "TestMain.h"

#include "http/HttpHandler.h"

TEST(http_handler_routes_request) {
    HttpHandler h;
    HttpAction a = h.handle("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(a.responses_.size(), size_t(1));
    CHECK(!a.close_);
    CHECK(!a.upgrade_);
}

TEST(http_handler_closes_when_client_asks_close) {
    HttpHandler h;
    HttpAction a = h.handle("GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
    CHECK_EQ(a.responses_.size(), size_t(1));
    CHECK(a.close_);
}

TEST(http_handler_keeps_connection_on_keep_alive) {
    HttpHandler h;
    HttpAction a = h.handle("GET / HTTP/1.1\r\nConnection: keep-alive\r\n\r\n");
    CHECK(!a.close_);
}

TEST(http_handler_does_not_treat_upgrade_as_close) {
    HttpHandler h;
    HttpAction a = h.handle(
        "GET /chat HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    CHECK(a.upgrade_);
    CHECK(!a.close_);
}

TEST(http_handler_closes_on_bad_request) {
    HttpHandler h;
    HttpAction a = h.handle("BADLINE\r\n\r\n");
    CHECK(a.close_);
}

TEST(http_handler_consumes_only_handled_bytes) {
    // 同段到达两个请求 未要求关闭时两个都处理
    HttpHandler h;
    HttpAction a = h.handle("GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n");
    CHECK_EQ(a.responses_.size(), size_t(2));
    CHECK_EQ(a.consumed_, size_t(36));
    CHECK(!a.close_);
}

TEST(http_handler_stops_at_close_request) {
    // 前一个请求要求关闭 后一个不再处理
    HttpHandler h;
    HttpAction a = h.handle("GET / HTTP/1.1\r\nConnection: close\r\n\r\nGET / HTTP/1.1\r\n\r\n");
    CHECK_EQ(a.responses_.size(), size_t(1));
    CHECK(a.close_);
}
