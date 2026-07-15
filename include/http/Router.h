// HTTP 路由分发 — 匹配路径 → 执行业务逻辑
#pragma once

#include <string>
#include <functional>
#include <unordered_map>

#include "./HttpRequest.h"
#include "./HttpResponse.h"

class Router {
public:
    Router();
    HttpResponse handle(const HttpRequest& req) const;

private:
    using HandlerFunc = std::function<HttpResponse(const HttpRequest&)>;

    std::unordered_map<std::string, HandlerFunc> routes_;

    void add(const std::string& path, HandlerFunc handler);
};
