// 静态文件服务 — 从磁盘读取文件并填充 HttpResponse
#include "http/StaticFileServer.h"
#include "http/ErrorResponse.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

// 根据扩展名推断 Content-Type
std::string mime_type(const std::string& path) {
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

// 静态资源的服务目录，请求路径解析后必须落在此目录之下
// 存规范化形式，与 lexically_normal 的输出同形才比得中
const std::string kRoot = "static";

// 百分号解码，串里出现非法转义就判定整条路径非法
bool url_decode(const std::string& in, std::string& out) {
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) {
            return false;
        }
        int hi = hex_val(in[i + 1]);
        int lo = hex_val(in[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

} 

std::string StaticFileServer::resolve(const std::string& request_path) {
    // 反斜杠在 Windows 上是路径分隔符，原始路径里字面出现即拒绝
    if (request_path.empty() || request_path[0] != '/' ||
        request_path.find('\\') != std::string::npos) {
        return {};
    }

    std::string decoded;
    if (!url_decode(request_path, decoded)) {
        return {};
    }
    // 空字节会让打开的文件名在此处截断，与校验时看到的不是同一个路径
    if (decoded.empty() || decoded[0] != '/' ||
        decoded.find('\0') != std::string::npos) {
        return {};
    }

    // 拼成 kRoot 之下的相对路径再规范化，.. 段在这一步被消掉
    std::string normalized = std::filesystem::path(kRoot + decoded).lexically_normal().string();
    // 规范化结果相对 kRoot 的位置，首段是 .. 说明 .. 段弹掉了 kRoot 本身，已越出服务目录
    std::filesystem::path rel = std::filesystem::path(normalized).lexically_relative(kRoot);
    if (rel.empty() || *rel.begin() == std::filesystem::path("..")) {
        return {};
    }
    // 规范化结果以目录分隔符续接时补回 ./，得到可直接打开的相对路径
    return normalized.size() > kRoot.size() ? "./" + normalized : normalized;
}

HttpResponse StaticFileServer::serve(const std::string& request_path) {
    HttpResponse resp;

    // 解析失败说明路径非法或越出服务目录
    std::string file_path = StaticFileServer::resolve(request_path);
    if (file_path.empty()) {
        return ErrorResponse::build_not_found(request_path);
    }

    // 目录与设备文件不是静态资源，按未找到处理
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file_path, ec)) {
        return ErrorResponse::build_not_found(request_path);
    }

    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return ErrorResponse::build_not_found(request_path);
    }

    // 定位失败按未找到处理
    std::streamsize size = file.tellg();
    if (size < 0) {
        return ErrorResponse::build_not_found(request_path);
    }
    // 超过上限直接拒绝，不允许大文件读进内存占着 io 线程
    if (static_cast<size_t>(size) > kMaxFileSize) {
        return ErrorResponse::build(HttpError::FILE_TOO_LARGE);
    }
    file.seekg(0, std::ios::beg);

    std::string buffer(static_cast<size_t>(size), '\0');
    if (!file.read(buffer.data(), size)) {
        return ErrorResponse::build(HttpError::READ_FAILED);
    }

    resp.status_ = 200;
    resp.status_text_ = "OK";
    resp.headers_.set("content-type", mime_type(file_path));
    resp.headers_.set("cache-control", "no-cache");
    resp.body_ = std::move(buffer);

    return resp;
}
