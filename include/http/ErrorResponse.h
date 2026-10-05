// 错误页面生成 — 把 HTTP 状态码包装成完整的 HTML 响应
#pragma once

#include <string>

#include "./HttpError.h"
#include "./HttpResponse.h"

class ErrorResponse {
public:
    // 按错误种类给出页面，状态码、状态文本与说明文案都按种类取
    static HttpResponse build(HttpError err);
    // 文案里要插文件路径，单独一个入口
    static HttpResponse build_not_found(const std::string& path);
};
