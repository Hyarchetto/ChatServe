// WebSocket 握手升级响应构造，RFC 6455
// 只做一件事：根据 HTTP Upgrade 请求构建 101 Switching Protocols 响应
#include "ws/WsUpgradeResponse.h"
#include "ws/SHA1.h"
#include "http/ErrorResponse.h"

// ==================== 握手升级响应 ====================

HttpResponse WsUpgradeResponse::build(const HttpRequest& req) {
    auto key = req.headers_.find("Sec-WebSocket-Key");
    if (!key) {
        return ErrorResponse::build_bad_request("Missing Sec-WebSocket-Key");
    }

    // RFC 6455 §4.2.2 含 Errata: SHA1 key + magic GUID 结果 base64 编码
    std::string combined = *key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
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
    resp.headers_.set("upgrade", "websocket");
    resp.headers_.set("connection", "Upgrade");
    resp.headers_.set("sec-websocket-accept", base64.substr(0, 28));
    return resp;
}
