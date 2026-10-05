// HTTP 路由分发
#include "http/HttpRouter.h"
#include "http/StaticFileServer.h"

HttpRouter::HttpRouter() {
    // 注册默认路由
    on("/", [](const HttpRequest&) -> HttpResponse {
        HttpResponse resp;
        resp.headers_.set("content-type", "text/html; charset=utf-8");
        resp.body_ = "<html><body><h1>ChatServe</h1><p>聊天服务器正在运行</p></body></html>";
        return resp;
    });

    // Vue 前端入口
    on("/chat", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("/index.html");
    });

    // 默认处理器，未匹配路径按静态文件兜底，从 static/ 目录读
    // 路径解析与越界判定都在 StaticFileServer 里
    on_default([](const HttpRequest& req) {
        return StaticFileServer::serve(req.path_);
    });
}

void HttpRouter::on(const std::string& path, Handler handler) {
    this->handlers_[path] = std::move(handler);
}

void HttpRouter::on_default(Handler handler) {
    this->default_handler_ = std::move(handler);
}

HttpResponse HttpRouter::handle(HttpRequest req) const {
    // 去除 query string，就地截断不重新分配
    if (auto qpos = req.path_.find('?'); qpos != std::string::npos) {
        req.path_.resize(qpos);
    }

    // 精确匹配
    if (auto it = this->handlers_.find(req.path_); it != this->handlers_.end()) {
        return it->second(req);
    }

    // 默认处理器兜底，按路径解析静态文件
    return this->default_handler_(req);
}
