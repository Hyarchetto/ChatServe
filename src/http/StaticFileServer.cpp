// 静态文件服务 — 读取文件并填充 HttpResponse
#include "http/StaticFileServer.h"

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

// ==================== 文件存在性检查 ====================

bool StaticFileServer::exists(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    return file.is_open();
}

// ==================== 静态文件服务 ====================

HttpResponse StaticFileServer::serve(const std::string& file_path) {
    HttpResponse resp;

    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return not_found(file_path);
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::string buffer(static_cast<size_t>(size), '\0');
    if (!file.read(buffer.data(), size)) {
        return server_error("读取文件失败");
    }

    resp.status_ = 200;
    resp.status_text_ = "OK";
    resp.headers_["Content-Type"] = mime_type(file_path);
    resp.body_ = std::move(buffer);

    return resp;
}

// ==================== 内部辅助 ====================

static HttpResponse make_error_page(int code, const std::string& text, const std::string& msg) {
    HttpResponse resp;
    resp.status_ = code;
    resp.status_text_ = text;
    resp.body_ = "<html><body><h1>" + std::to_string(code) + " " + text + "</h1><p>" + msg + "</p></body></html>";
    resp.headers_["Content-Type"] = "text/html; charset=utf-8";
    return resp;
}

// ==================== 错误页面 ====================

HttpResponse StaticFileServer::not_found(const std::string& path) {
    std::string msg = "路径 " + path + " 未找到";
    return make_error_page(404, "Not Found", msg);
}

HttpResponse StaticFileServer::bad_request(const std::string& msg) {
    return make_error_page(400, "Bad Request", msg);
}

HttpResponse StaticFileServer::server_error(const std::string& msg) {
    return make_error_page(500, "Internal Server Error", msg);
}

HttpResponse StaticFileServer::method_not_allowed(const std::string& msg) {
    return make_error_page(405, "Method Not Allowed", msg);
}

// ==================== 重定向 ====================

HttpResponse StaticFileServer::redirect(const std::string& location) {
    HttpResponse resp;
    resp.status_ = 302;
    resp.status_text_ = "Found";
    resp.headers_["Location"] = location;
    return resp;
}