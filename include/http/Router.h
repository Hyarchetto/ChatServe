// HTTP 路由分发 — 匹配路径 → 执行业务逻辑
#pragma once

#include <string>
#include <functional>
#include <unordered_map>

#include "./HttpRequest.h"
#include "./HttpResponse.h"

class FileManager;

class Router {
public:
    // 传 FileManager 进来，文件下载也走路由，不用 Reactor 硬编码路径
    Router(FileManager* file_mgr);
    // 路由分发，返回 HttpResponse；无匹配返回 404
    HttpResponse handle(const HttpRequest& req) const;

private:
    using HandlerFunc = std::function<HttpResponse(const HttpRequest&)>;

    std::unordered_map<std::string, HandlerFunc> routes_;  // 路由表
    FileManager* file_mgr_;                                // 文件管理器，处理 /files/ 下载

    void add(const std::string& path, HandlerFunc handler); // 注册路由
    HttpResponse handle_file_download(const std::string& path) const;
};
