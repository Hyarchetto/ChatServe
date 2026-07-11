// HTTP 路由分发 — 匹配路径 → 执行业务逻辑
// 所有 HTTP 请求都走这里，包括文件下载
#include "http/Router.h"
#include "http/StaticFileServer.h"
#include "http/ErrorResponse.h"
#include "filetransfer/FileManager.h"

Router::Router(FileManager* file_mgr) : file_mgr_(file_mgr) {
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

    // 精确匹配
    auto it = this->routes_.find(path);
    if (it != this->routes_.end()) {
        return it->second(req);
    }

    // 文件下载，路径格式 /files/<file_id>
    if (path.rfind("/files/", 0) == 0) {
        return this->handle_file_download(path);
    }

    // 静态文件 fallback: 路径去除前导 / 后检查文件是否存在
    {
        std::string file_path = path.substr(1);
        if (StaticFileServer::exists(file_path)) {
            return StaticFileServer::serve(file_path);
        }
    }

    // 没找到返回 404
    return ErrorResponse::not_found(path);
}

HttpResponse Router::handle_file_download(const std::string& path) const {
    std::string remaining = path.substr(7); // 去掉 "/files/"
    auto slash = remaining.find('/');
    std::string file_id = remaining.substr(0, slash);

    std::string file_data;
    if (!this->file_mgr_->read_file(file_id, file_data)) {
        return ErrorResponse::not_found(path);
    }

    HttpResponse resp;
    resp.status_ = 200;
    resp.status_text_ = "OK";
    resp.headers_["Content-Type"] = "application/octet-stream";
    resp.headers_["Content-Disposition"] = "attachment";
    resp.headers_["Content-Length"] = std::to_string(file_data.size());
    resp.body_ = std::move(file_data);
    return resp;
}

void Router::add(const std::string& path, HandlerFunc handler) {
    this->routes_[path] = std::move(handler);
}
