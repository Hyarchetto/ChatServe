// WebSocket 握手升级响应构造（RFC 6455）
// 本文件只做一件事：根据 HTTP Upgrade 请求构建 101 Switching Protocols 响应
#pragma once

#include "../http/HttpRequest.h"
#include "../http/HttpResponse.h"

// WebSocket 升级响应构造器
class WebSocketUpgradeResponse {
public:
    WebSocketUpgradeResponse() = delete;

    // 根据 Upgrade 请求构建 101 响应，key 缺失则返回 400
    static HttpResponse build(const HttpRequest& req);
};
