// 静态文件服务 — 从磁盘读取文件并填充 HttpResponse
// 错误页面的生成和重定向不在这里
#include "http/StaticFileServer.h"
#include "http/ErrorResponse.h"

// 根据扩展名推断 Content-Type
static std::string mime_type(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot);
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css")  return "text/css; charset=utf-8";
    if (ext == ".js")   return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".txt")  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

HttpResponse StaticFileServer::serve(const std::string& file_path) {
    HttpResponse resp;

    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return ErrorResponse::not_found(file_path);
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::string buffer(static_cast<size_t>(size), '\0');
    if (!file.read(buffer.data(), size)) {
        return ErrorResponse::server_error("读取文件失败");
    }

    resp.status_ = 200;
    resp.status_text_ = "OK";
    resp.headers_["Content-Type"] = mime_type(file_path);
    resp.body_ = std::move(buffer);

    return resp;
}