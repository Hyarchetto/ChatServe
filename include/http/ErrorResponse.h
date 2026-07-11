// 错误页面生成 — 把 HTTP 状态码包装成完整的 HTML 响应
// 和 StaticFileServer 分开，各管各的
#pragma once

#include <string>

#include "./HttpResponse.h"

class ErrorResponse {
public:
    static HttpResponse not_found(const std::string& path = "");
    static HttpResponse bad_request(const std::string& msg);
    static HttpResponse server_error(const std::string& msg);
    static HttpResponse method_not_allowed(const std::string& msg);
};
