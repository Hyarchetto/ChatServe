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

static HttpResponse make_error_page(int code, const std::string& text, const std::string& msg) {
    HttpResponse resp;
    resp.status_ = code;
    resp.status_text_ = text;
    resp.body_ = "<html><body><h1>" + std::to_string(code) + " " + escape_html(text)
               + "</h1><p>" + escape_html(msg) + "</p></body></html>";
    resp.headers_["Content-Type"] = "text/html; charset=utf-8";
    return resp;
}

HttpResponse ErrorResponse::not_found(const std::string& path) {
    std::string msg = "路径 " + path + " 未找到";
    return make_error_page(404, "Not Found", msg);
}

HttpResponse ErrorResponse::bad_request(const std::string& msg) {
    return make_error_page(400, "Bad Request", msg);
}

HttpResponse ErrorResponse::server_error(const std::string& msg) {
    return make_error_page(500, "Internal Server Error", msg);
}

