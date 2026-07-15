// WebSocket 握手升级响应构造，RFC 6455
// 本文件只做一件事：根据 HTTP Upgrade 请求构建 101 Switching Protocols 响应
#include "ws/WebSocketUpgradeResponse.h"
#include "ws/SHA1.h"

// ==================== 握手升级响应 ====================

HttpResponse WebSocketUpgradeResponse::build(const HttpRequest& req) {
    std::string key = req.header("Sec-WebSocket-Key");
    if (key.empty()) {
        HttpResponse resp;
        resp.status_ = 400;
        resp.status_text_ = "Bad Request";
        resp.headers_["Content-Type"] = "text/html; charset=utf-8";
        resp.body_ = "<html><body><h1>400 Bad Request</h1><p>Missing Sec-WebSocket-Key</p></body></html>";
        return resp;
    }

    // RFC 6455 §4.2.2 (with Errata): SHA1(key + magic GUID) → base64
    std::string combined = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t hash[20];
    SHA1::hash(reinterpret_cast<const uint8_t*>(combined.data()), combined.size(), hash);

    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string base64;
    base64.reserve(28);
    for (int i = 0; i < 20; i += 3) {
        int n = 20 - i;
        unsigned long triple = (static_cast<unsigned long>(hash[i]) << 16)
                             | (static_cast<unsigned long>(hash[i+1]) << 8)
                             | (n > 2 ? hash[i+2] : 0);
        base64.push_back(b64[(triple >> 18) & 0x3F]);
        base64.push_back(b64[(triple >> 12) & 0x3F]);
        if (n > 1) base64.push_back(b64[(triple >> 6) & 0x3F]);
        if (n > 2) base64.push_back(b64[triple & 0x3F]);
    }
    base64.append((4 - base64.size() % 4) % 4, '=');

    HttpResponse resp;
    resp.status_ = 101;
    resp.status_text_ = "Switching Protocols";
    resp.headers_["Upgrade"] = "websocket";
    resp.headers_["Connection"] = "Upgrade";
    resp.headers_["Sec-WebSocket-Accept"] = base64.substr(0, 28);
    return resp;
}
