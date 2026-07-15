// HTTP 路由分发 — 匹配路径 → 执行业务逻辑
#include "http/Router.h"
#include "http/StaticFileServer.h"
#include "http/ErrorResponse.h"

Router::Router() {
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
    add("/app.js", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/app.js");
    });
    add("/style.css", [](const HttpRequest&) -> HttpResponse {
        return StaticFileServer::serve("./static/chat/style.css");
    });
    add("/hello", [](const HttpRequest&) -> HttpResponse {
        HttpResponse resp;
        resp.headers_["Content-Type"] = "text/html; charset=utf-8";
        resp.body_ = "<html><body><h1>Hello World!</h1></body></html>";
        return resp;
    });
}

HttpResponse Router::handle(const HttpRequest& req) const {
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

void Router::add(const std::string& path, HandlerFunc handler) {
    this->routes_[path] = std::move(handler);
}
