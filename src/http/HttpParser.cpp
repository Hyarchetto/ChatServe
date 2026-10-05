// HTTP 请求解析器 — 无状态，按调用方给的进度续解析
#include "http/HttpParser.h"

#include <algorithm>
#include <cctype>
#include <string_view>

// ASCII 大小写不敏感比较，两侧等长且逐字符同小写即相同
static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Connection 头的值是逗号分隔的 token 列表，判断其中是否含指定 token
// 大小写不敏感，两侧空白与空 token 都跳过
// 头值不能就地改大小写，Sec-WebSocket-Key 的 base64 就大小写敏感
static bool has_conn_token(const std::string& value, std::string_view want) {
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(',', start);
        if (end == std::string::npos) {
            end = value.size();
        }
        std::string_view token(value.data() + start, end - start);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }
        if (iequals(token, want)) {
            return true;
        }
        if (end == value.size()) {
            break;
        }
        start = end + 1;
    }
    return false;
}

// 请求体长度上限，没有消费它的路径，取 1MB 限住每连接最坏情况
static constexpr size_t kMaxBodyBytes = 1024 * 1024;

// 判错收尾，状态清回起点
// 判错回包冲刷完前读事件还能再进来，残留字节那时按新请求解析
static HttpResult reject(HttpRequestState& st, size_t consumed, HttpError err) {
    HttpResult result;
    result.type_ = HttpResultType::BAD_REQUEST;
    result.error_ = err;
    result.consumed_ = consumed;
    st = HttpRequestState{};
    return result;
}

// 从 pos 起跳过空白取一段写进 out，取不到返回 false
// 这一段取到下一个空白，之后没有空白就取到行尾
static bool take_token(std::string_view line, size_t& pos, std::string_view& out) {
    size_t begin = line.find_first_not_of(" \t", pos);
    if (begin == std::string_view::npos) {
        return false;
    }
    size_t end = line.find_first_of(" \t", begin);
    out = line.substr(begin, end - begin);
    pos = (end == std::string_view::npos) ? line.size() : end;
    return true;
}

// 按空白切出方法、路径、版本三段写进 req，少于三段返回 REQUEST_LINE_SYNTAX
// 空白只认 SP 与 HTAB，RFC 7230 的 OWS 就这两个
static HttpError parse_request_line(std::string_view line, HttpRequest& req) {
    size_t pos = 0;
    std::string_view token;
    if (!take_token(line, pos, token)) return HttpError::REQUEST_LINE_SYNTAX;
    req.method_ = token;
    if (!take_token(line, pos, token)) return HttpError::REQUEST_LINE_SYNTAX;
    req.path_ = token;
    // 版本取到下一个空白，其后的多余字段一律忽略
    if (!take_token(line, pos, token)) return HttpError::REQUEST_LINE_SYNTAX;
    req.version_ = token;
    return HttpError::NONE;
}

// 解析一条头字段写入头表，合法返回 HttpError::NONE
// 冒号前的空白是畸形写法，留着会让头名和查找用的名字对不上，整个头被静默忽略
// 头值两侧的 OWS 都要裁掉，RFC 7230 3.2.4 规定服务端必须忽略
static HttpError parse_header(HeaderMap& headers, std::string_view line) {
    auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return HttpError::HEADER_SYNTAX;
    }
    std::string_view key = line.substr(0, colon);
    size_t key_end = key.find_last_not_of(" \t");
    if (key_end == std::string_view::npos) {
        return HttpError::HEADER_SYNTAX;
    }
    // 大小写归一化由头表负责，这里只管裁掉两侧空白
    key = key.substr(0, key_end + 1);

    std::string_view val = line.substr(colon + 1);
    size_t first = val.find_first_not_of(" \t");
    size_t last = val.find_last_not_of(" \t");
    val = (first == std::string_view::npos) ? std::string_view{} : val.substr(first, last - first + 1);

    // 重复的 Content-Length 中间层与服务器可能各取一个，直接判错
    // 其余头名重复是 RFC 7230 允许的，后写的覆盖先写的
    if (!headers.set(key, val) && iequals(key, "content-length")) {
        return HttpError::DUPLICATE_LENGTH;
    }
    return HttpError::NONE;
}

// 解析 Content-Length 的值，合法写入 out 并返回 HttpError::NONE
// 整串必须是十进制数字，逐位累加，越限即停
static HttpError parse_content_length(const std::string& val, size_t& out) {
    if (val.empty()) {
        return HttpError::INVALID_LENGTH;
    }
    size_t n = 0;
    for (char c : val) {
        if (c < '0' || c > '9') {
            return HttpError::INVALID_LENGTH;
        }
        n = n * 10 + static_cast<size_t>(c - '0');
        if (n > kMaxBodyBytes) {
            return HttpError::BODY_TOO_LARGE;
        }
    }
    out = n;
    return HttpError::NONE;
}

HttpResult HttpParser::handle(std::string_view buf, HttpRequestState& st) {
    HttpResult result;
    size_t pos = 0;          // 本次调用已消费的字节数
    std::string_view line;   // 逐行指向读缓冲的视图，本调用内有效

    while (true) {
        switch (st.phase_) {
            // ==================== 请求行 ====================
            case HttpPhase::REQUEST_LINE: {
                if (!read_line(buf, pos, line)) {
                    // 行未终结且累计越限，说明头部在无界增长
                    if (st.header_bytes_ + (buf.size() - pos) > kMaxHeaderBytes) {
                        return reject(st, pos, HttpError::HEADER_TOO_LARGE);
                    }
                    result.consumed_ = pos;
                    return result;  // INCOMPLETE
                }
                if (HttpError err = parse_request_line(line, st.request_); err != HttpError::NONE) {
                    return reject(st, pos, err);
                }
                // 只认 GET 与 POST
                if (st.request_.method_ != "GET" && st.request_.method_ != "POST") {
                    return reject(st, pos, HttpError::METHOD);
                }
                st.header_bytes_ += pos;
                st.phase_ = HttpPhase::HEADERS;
                break;
            }

            // ==================== 请求头 ====================
            case HttpPhase::HEADERS: {
                const size_t line_begin = pos;
                const bool got_line = read_line(buf, pos, line);
                // 算上这一行，头部已读字节数越过上限即拒
                // 未终结的那一行整段还在缓冲里，长度就是缓冲的剩余
                const size_t pending = got_line ? pos - line_begin : buf.size() - pos;
                if (st.header_bytes_ + pending > kMaxHeaderBytes) {
                    return reject(st, pos, HttpError::HEADER_TOO_LARGE);
                }
                if (!got_line) {
                    result.consumed_ = pos;
                    return result;  // INCOMPLETE
                }
                st.header_bytes_ += pos - line_begin;
                // 空行 → 头部结束
                if (line.empty()) {
                    // 出现 Transfer-Encoding 一律拒，本服务端只按 Content-Length 定长读体
                    // RFC 7230 3.3.3 规定有 Transfer-Encoding 时 Content-Length 必须忽略或拒，两者并存会让请求错位
                    if (st.request_.headers_.find("Transfer-Encoding")) {
                        return reject(st, pos, HttpError::TRANSFER_ENCODING);
                    }
                    // 头表查找大小写不敏感，精确匹配会漏掉 content-length 变体 body 不消耗导致请求错位
                    if (auto cl = st.request_.headers_.find("Content-Length")) {
                        if (HttpError err = parse_content_length(*cl, st.content_length_); err != HttpError::NONE) {
                            return reject(st, pos, err);
                        }
                    }
                    st.phase_ = HttpPhase::BODY;
                    break;
                }
                if (HttpError err = parse_header(st.request_.headers_, line); err != HttpError::NONE) {
                    return reject(st, pos, err);
                }
                break;
            }

            // ==================== 请求体与完成 ====================
            case HttpPhase::BODY: {
                const size_t want = st.content_length_ - st.request_.body_.size();
                const size_t take = std::min(want, buf.size() - pos);
                st.request_.body_.append(buf.data() + pos, take);
                pos += take;
                if (st.request_.body_.size() < st.content_length_) {
                    result.consumed_ = pos;
                    return result;  // INCOMPLETE
                }
                auto upgrade = st.request_.headers_.find("Upgrade");
                auto connection_hdr = st.request_.headers_.find("Connection");
                if (upgrade && iequals(*upgrade, "websocket") &&
                    connection_hdr && has_conn_token(*connection_hdr, "upgrade")) {
                    result.type_ = HttpResultType::WS_UPGRADE;
                }
                else {
                    result.type_ = HttpResultType::OK;
                }
                // 客户端要求关闭连接，回应后由施加侧断开
                result.close_ = connection_hdr && has_conn_token(*connection_hdr, "close");
                result.consumed_ = pos;
                result.request_ = std::move(st.request_);
                st = HttpRequestState{};
                return result;
            }
        }
    }
}

bool HttpParser::read_line(std::string_view buf, size_t& pos, std::string_view& line) {
    auto n = buf.find("\r\n", pos);
    if (n == std::string::npos) return false;
    line = buf.substr(pos, n - pos);
    pos = n + 2;
    return true;
}
