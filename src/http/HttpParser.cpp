// HTTP 请求解析器 — 无状态，每次从头解析完整 buffer
// 从 TCP buffer 中解析出 HTTP 请求
#include "http/HttpParser.h"

bool HttpParser::read_line(const std::string& buf, size_t& pos, std::string& line) {
    auto n = buf.find("\r\n", pos);
    if (n == std::string::npos) return false;
    line = buf.substr(pos, n - pos);
    pos = n + 2;
    return true;
}

HttpResult HttpParser::handle(const std::string& buf) {
    HttpResult result;
    size_t pos = 0;

    // ==================== 请求行 ====================
    {
        std::string line;
        if (!read_line(buf, pos, line)) {
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
        if (!read_line(buf, pos, line)) {
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
        while (!val.empty() && val[0] == ' ') val.erase(0, 1);
        result.request_.headers_[key] = val;
    }

    // ==================== 请求体 ====================
    {
        size_t content_length = 0;
        if (auto cl_it = result.request_.headers_.find("Content-Length"); cl_it != result.request_.headers_.end()) {
            try {
                content_length = std::stoul(cl_it->second);
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
        auto upgrade = result.request_.header("Upgrade");
        auto connection_hdr = result.request_.header("Connection");
        if (upgrade == "websocket" && connection_hdr.find("Upgrade") != std::string::npos) {
            result.type_ = HttpResultType::WS_UPGRADE;
        } 
        else {
            result.type_ = HttpResultType::OK;
        }
        result.finished_ = pos;
        return result;
    }
}
