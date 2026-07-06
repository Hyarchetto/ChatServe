// HTTP 路由分发 — 匹配路径 → 执行业务逻辑
#include "http/Router.h"
#include "http/StaticFileServer.h"

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
    auto qpos = path.find('?');
    if (qpos != std::string::npos) {
        path = path.substr(0, qpos);
    }

    // 匹配
    auto it = this->routes_.find(path);
    if (it != this->routes_.end()) {
        return it->second(req);
    }

    // 静态文件 fallback: 路径去除前导 / 后检查文件是否存在
    {
        std::string file_path = path.substr(1);
        if (StaticFileServer::exists(file_path)) {
            return StaticFileServer::serve(file_path);
        }
    }

    // 没找到返回 404
    return StaticFileServer::not_found(path);
}

void Router::add(const std::string& path, HandlerFunc handler) {
    this->routes_[path] = std::move(handler);
}
