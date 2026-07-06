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
    // 路由分发，返回 HttpResponse；无匹配返回 404
    HttpResponse handle(const HttpRequest& req) const;

private:
    using HandlerFunc = std::function<HttpResponse(const HttpRequest&)>;
    // ============================ 私有成员 ============================
    std::unordered_map<std::string, HandlerFunc> routes_;  // 路由表
    void add(const std::string& path, HandlerFunc handler); // 注册路由
};
