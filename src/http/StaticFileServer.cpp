// 静态文件服务 — 从磁盘读取文件并填充 HttpResponse
// 错误页面的生成和重定向不在这里
#include "http/StaticFileServer.h"
#include "http/ErrorResponse.h"

#include <filesystem>
#include <fstream>
#include <string>

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

}  // namespace

std::string StaticFileServer::resolve(const std::string& request_path) {
    // 反斜杠在 Windows 上是路径分隔符，解码后可能变成文件系统分隔符，一律拒绝
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
    std::string normalized = std::filesystem::path(kRoot + decoded)
                                 .lexically_normal().string();
    // rfind 命中前缀时返回 0，非 0 即不以服务目录开头，越界的路径在这里被挡下
    if (normalized.rfind(kRoot, 0) != 0) {
        return {};
    }
    // 规范化结果以目录分隔符续接时补回 ./，得到可直接打开的相对路径
    return normalized.size() > kRoot.size() ? "./" + normalized : normalized;
}

HttpResponse StaticFileServer::serve(const std::string& file_path) {
    HttpResponse resp;

    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return ErrorResponse::build_not_found(file_path);
    }

    // 取不到大小说明不是普通文件，目录与设备文件都走这里
    std::streamsize size = file.tellg();
    if (size < 0) {
        return ErrorResponse::build_not_found(file_path);
    }
    // 超过上限直接拒绝，不把一个可能很大的文件读进内存占着 io 线程
    if (static_cast<size_t>(size) > kMaxFileSize) {
        return ErrorResponse::build_payload_too_large("文件超过单文件上限");
    }
    file.seekg(0, std::ios::beg);

    std::string buffer(static_cast<size_t>(size), '\0');
    if (!file.read(buffer.data(), size)) {
        return ErrorResponse::build_server_error("读取文件失败");
    }

    resp.status_ = 200;
    resp.status_text_ = "OK";
    resp.headers_["content-type"] = mime_type(file_path);
    resp.headers_["cache-control"] = "no-cache";
    resp.body_ = std::move(buffer);

    return resp;
}
