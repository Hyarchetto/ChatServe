// HTTP 路由分发 — 匹配路径 → 执行业务逻辑
#include "http/HttpRouter.h"
#include "http/StaticFileServer.h"
#include "http/ErrorResponse.h"

HttpRouter::HttpRouter() {
    // 注册默认路由
    on("/", [](const HttpRequest&) -> HttpResponse {
        HttpResponse resp;
        resp.headers_["Content-Type"] = "text/html; charset=utf-8";
        resp.body_ = "<html><body><h1>ChatServe</h1><p>聊天服务器正在运行</p></body></html>";
        return resp;
    });

    // Vue 前端入口
    on("/chat", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve(StaticFileServer::resolve("/index.html"));
    });

    // 默认处理器 未匹配路径按静态文件兜底 从 static/ 目录读
    // 路径解析交给 StaticFileServer，越出服务目录的一律当不存在
    on_default([](const HttpRequest& req) {
        std::string file_path = StaticFileServer::resolve(req.path_);
        if (file_path.empty()) {
            return ErrorResponse::build_not_found(req.path_);
        }
        return StaticFileServer::serve(file_path);
    });
}

void HttpRouter::on(const std::string& path, Handler handler) {
    this->handlers_[path] = std::move(handler);
}

void HttpRouter::on_default(Handler handler) {
    this->default_handler_ = std::move(handler);
}

HttpResponse HttpRouter::handle(const HttpRequest& req) const {
    std::string path = req.path_;

    // 去除 query string
    if (auto qpos = path.find('?'); qpos != std::string::npos) {
        path = path.substr(0, qpos);
    }

    // 精确匹配
    if (auto it = this->handlers_.find(path); it != this->handlers_.end()) {
        return it->second(req);
    }

    // 默认处理器兜底，传入已剥离 query 的请求副本
    HttpRequest normalized = req;
    normalized.path_ = path;
    return this->default_handler_(normalized);
}
