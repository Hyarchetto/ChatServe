// 错误页面实现
#include "http/ErrorResponse.h"

static std::string escape_html(const std::string& input) {
    std::string out;
    for (char c : input) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

static HttpResponse build_error_page(int code, const std::string& text, const std::string& msg) {
    HttpResponse resp;
    resp.status_ = code;
    resp.status_text_ = text;
    resp.body_ = "<html><body><h1>" + std::to_string(code) + " " + escape_html(text)
               + "</h1><p>" + escape_html(msg) + "</p></body></html>";
    resp.headers_.set("content-type", "text/html; charset=utf-8");
    return resp;
}

// 每种错误的状态码、状态文本与说明文案都收在这一处
// 状态文本进状态行，是协议固定的英文原因短语，说明文案进页面正文
// 每个枚举值各自 return，新种类漏写会被 -Wswitch 点名
HttpResponse ErrorResponse::build(HttpError err) {
    switch (err) {
        case HttpError::REQUEST_LINE_SYNTAX:
            return build_error_page(400, "Bad Request", "请求行不完整");
        case HttpError::METHOD:
            return build_error_page(400, "Bad Request", "只支持 GET 与 POST");
        case HttpError::HEADER_SYNTAX:
            return build_error_page(400, "Bad Request", "请求头格式错误");
        case HttpError::DUPLICATE_LENGTH:
            return build_error_page(400, "Bad Request", "重复的 Content-Length");
        case HttpError::TRANSFER_ENCODING:
            return build_error_page(400, "Bad Request", "不支持的 Transfer-Encoding");
        case HttpError::INVALID_LENGTH:
            return build_error_page(400, "Bad Request", "Content-Length 格式错误");
        case HttpError::HEADER_TOO_LARGE:
            return build_error_page(431, "Request Header Fields Too Large", "请求头超过上限");
        case HttpError::BODY_TOO_LARGE:
            return build_error_page(413, "Payload Too Large", "请求体超过上限");
        case HttpError::MISSING_WS_KEY:
            return build_error_page(400, "Bad Request", "升级请求缺少 Sec-WebSocket-Key");
        case HttpError::FILE_TOO_LARGE:
            return build_error_page(413, "Payload Too Large", "文件超过单文件上限");
        case HttpError::READ_FAILED:
            return build_error_page(500, "Internal Server Error", "读取文件失败");
        // 调用方传默认值给通用页
        case HttpError::NONE:
            return build_error_page(400, "Bad Request", "请求无效");
    }
    // 上面已覆盖 HttpError 的全部取值
    __builtin_unreachable();
}

HttpResponse ErrorResponse::build_not_found(const std::string& path) {
    std::string msg = "路径 " + path + " 未找到";
    return build_error_page(404, "Not Found", msg);
}
