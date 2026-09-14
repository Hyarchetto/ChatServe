// HTTP 请求解析器 — 无状态，每次从头解析完整 buffer
// 从 TCP buffer 中解析出 HTTP 请求
#include "http/HttpParser.h"

#include <string_view>
#include <sstream>

// 大小写不敏感的子串包含 逐位退化成等长比较 命中任一位置即真
static bool icontains(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (HttpRequest::ieq(haystack.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

// 请求行与请求头的累计上限 防客户端无限发头撑爆读缓冲
// 取 64KB 常见浏览器头部不足 8KB 留足余量 与 ws 侧的帧上限对称
static constexpr size_t kMaxHeaderBytes = 64 * 1024;

// 头部超限 直接判定为错误请求
static HttpResult build_header_too_large() {
    HttpResult result;
    result.type_ = HttpResultType::BAD_REQUEST;
    result.error_msg_ = "Header too large";
    return result;
}

// 行未终结时缓冲区仍超上限 说明头部在无界增长
// 只在 read_line 失败时可用 此时缓冲区必然全是尚未终结的头部
// 若请求本体已到达 头部终结符必然已被读到 不会走到这里
static bool has_unterminated_overflow(std::string_view buf) {
    return buf.size() > kMaxHeaderBytes;
}

HttpResult HttpParser::handle(std::string_view buf) {
    HttpResult result;
    size_t pos = 0;

    // ==================== 请求行 ====================
    {
        std::string line;
        if (!read_line(buf, pos, line)) {
            if (has_unterminated_overflow(buf)) {
                return build_header_too_large();
            }
            return result;  // INCOMPLETE
        }
        if (std::istringstream iss(line); !(iss >> result.request_.method_ >> result.request_.path_ >> result.request_.version_)) {
            result.type_ = HttpResultType::BAD_REQUEST;
            result.error_msg_ = "Invalid request line";
            return result;
        }
        if (result.request_.method_ != "GET" && result.request_.method_ != "POST") {
            result.type_ = HttpResultType::BAD_REQUEST;
            result.error_msg_ = "Only GET and POST are supported";
            return result;
        }
    }

    // ==================== 请求头 ====================
    std::string line;
    while (true) {
        // 行已终结但累计超限 每行都很短也能撑爆
        if (pos > kMaxHeaderBytes) {
            return build_header_too_large();
        }
        if (!read_line(buf, pos, line)) {
            if (has_unterminated_overflow(buf)) {
                return build_header_too_large();
            }
            return result;  // INCOMPLETE
        }
        // 空行 → 头部结束
        if (line.empty()) break;

        auto colon = line.find(':');
        if (colon == std::string::npos) {
            result.type_ = HttpResultType::BAD_REQUEST;
            result.error_msg_ = "Invalid header";
            return result;
        }
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // RFC 7230 的 OWS 同时包含空格与水平制表符，两个都要去掉
        size_t first = val.find_first_not_of(" \t");
        val = (first == std::string::npos) ? std::string{} : val.substr(first);
        result.request_.headers_[key] = val;
    }

    // ==================== 请求体 ====================
    {
        size_t content_length = 0;
        // 走大小写容错查找，精确匹配会漏掉 content-length 变体，body 不消耗导致请求错位
        auto cl = result.request_.find_header("Content-Length");
        if (cl) {
            try {
                content_length = std::stoul(*cl);
            }
            catch (...) {
                result.type_ = HttpResultType::BAD_REQUEST;
                result.error_msg_ = "Invalid Content-Length";
                return result;
            }
            if (content_length > 10 * 1024 * 1024) {
                result.type_ = HttpResultType::BAD_REQUEST;
                result.error_msg_ = "Request body too large";
                return result;
            }
        }
        if (content_length > 0) {
            size_t available = buf.size() - pos;
            if (available < static_cast<size_t>(content_length)) {
                return result;  // INCOMPLETE
            }
            result.request_.body_ = buf.substr(pos, content_length);
            pos += content_length;
        }
    }

    // ==================== 完成 ====================
    {
        auto upgrade = result.request_.find_header("Upgrade");
        auto connection_hdr = result.request_.find_header("Connection");
        if (upgrade && HttpRequest::ieq(*upgrade, "websocket") &&
            connection_hdr && icontains(*connection_hdr, "Upgrade")) {
            result.type_ = HttpResultType::WS_UPGRADE;
        } 
        else {
            result.type_ = HttpResultType::OK;
        }
        result.consumed_ = pos;
        return result;
    }
}

bool HttpParser::read_line(std::string_view buf, size_t& pos, std::string& line) {
    auto n = buf.find("\r\n", pos);
    if (n == std::string::npos) return false;
    line = buf.substr(pos, n - pos);
    pos = n + 2;
    return true;
}
