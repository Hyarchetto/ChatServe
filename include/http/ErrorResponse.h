// 错误页面生成 — 把 HTTP 状态码包装成完整的 HTML 响应
// 和 StaticFileServer 分开，各管各的
#pragma once

#include <string>

#include "./HttpResponse.h"

class ErrorResponse {
public:
    static HttpResponse build_not_found(const std::string& path = "");
    static HttpResponse build_bad_request(const std::string& msg);
    static HttpResponse build_payload_too_large(const std::string& msg);
    static HttpResponse build_server_error(const std::string& msg);
};
