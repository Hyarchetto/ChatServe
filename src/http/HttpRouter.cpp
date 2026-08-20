// HTTP 路由分发 — 匹配路径 → 执行业务逻辑
#include "http/HttpRouter.h"
#include "http/StaticFileServer.h"

HttpRouter::HttpRouter() {
    // 注册默认路由
    on("/", [](const HttpRequest&) -> HttpResponse {
        HttpResponse resp;
        resp.headers_["Content-Type"] = "text/html; charset=utf-8";
        resp.body_ = "<html><body><h1>ChatServe</h1><p>聊天服务器正在运行</p></body></html>";
        return resp;
    });

    // 静态文件
    on("/chat", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/chat.html");
    });
    on("/js/app.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/app.js");
    });
    on("/js/state.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/state.js");
    });
    on("/js/utils.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/utils.js");
    });
    on("/js/ui.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/ui.js");
    });
    on("/js/connection.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/connection.js");
    });
    on("/js/webrtc.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/webrtc.js");
    });
    on("/js/file-transfer.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/file-transfer.js");
    });
    on("/js/protocol.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/protocol.js");
    });
    on("/css/style.css", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/css/style.css");
    });

    // 默认处理器，未匹配路径时按静态文件兜底
    on_default([](const HttpRequest& req) {
        return StaticFileServer::serve(req.path_.substr(1));
    });
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

void HttpRouter::on(const std::string& path, Handler handler) {
    this->handlers_[path] = std::move(handler);
}

void HttpRouter::on_default(Handler handler) {
    this->default_handler_ = std::move(handler);
}
