// HTTP 路由分发 — 匹配路径 → 执行业务逻辑
#pragma once

#include <string>
#include <functional>
#include <unordered_map>

#include "./HttpRequest.h"
#include "./HttpResponse.h"

class HttpRouter {
public:
    HttpRouter();

    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    // 注册路径处理器
    void on(const std::string& path, Handler handler);

    // 注册默认处理器，未匹配路径时兜底
    void on_default(Handler handler);

    // 分发请求，返回响应
    HttpResponse handle(const HttpRequest& req) const;

private:
    std::unordered_map<std::string, Handler> handlers_;
    Handler default_handler_;
};
