// HTTP 路由分发 — 匹配路径 → 执行业务逻辑
#include "http/HttpRouter.h"
#include "http/StaticFileServer.h"
#include "http/ErrorResponse.h"

HttpRouter::HttpRouter() {
    // 注册默认路由
    add("/", [](const HttpRequest&) -> HttpResponse {
        HttpResponse resp;
        resp.headers_["Content-Type"] = "text/html; charset=utf-8";
        resp.body_ = "<html><body><h1>ChatServe</h1><p>聊天服务器正在运行</p></body></html>";
        return resp;
    });

    // 静态文件
    add("/chat", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/chat.html");
    });
    add("/js/app.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/app.js");
    });
    add("/js/state.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/state.js");
    });
    add("/js/utils.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/utils.js");
    });
    add("/js/ui.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/ui.js");
    });
    add("/js/connection.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/connection.js");
    });
    add("/js/webrtc.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/webrtc.js");
    });
    add("/js/file-transfer.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/file-transfer.js");
    });
    add("/js/protocol.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/js/protocol.js");
    });
    add("/css/style.css", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/css/style.css");
    });
}

HttpResponse HttpRouter::handle(const HttpRequest& req) const {
    std::string path = req.path_;

    // 去除 query string
    if (auto qpos = path.find('?'); qpos != std::string::npos) {
        path = path.substr(0, qpos);
    }

    // 精确匹配
    if (auto it = this->routes_.find(path); it != this->routes_.end()) {
        return it->second(req);
    }

    // 静态文件 fallback，serve 内部已处理文件不存在的情况
    {
        return StaticFileServer::serve(path.substr(1));
    }

}

void HttpRouter::add(const std::string& path, HandlerFunc handler) {
    this->routes_[path] = std::move(handler);
}
