// HTTP 响应结构，serialize() 生成线路数据
#pragma once

#include <string>

#include "HeaderMap.h"

struct HttpResponse {
    int status_ = 200;
    std::string status_text_ = "OK";
    std::string version_ = "HTTP/1.1";
    HeaderMap headers_;                              // 头名大小写不敏感，键以归一化形态存储
    std::string body_;

    // 标准序列化：状态行 + 头部 + 空行 + body
    // 手工拼接，body 只拷一次，不经流的缓冲与二次 crate
    std::string serialize() const {
        // 调用方未显式设置 Content-Length 时才自动计算
        // 1xx 与 204 没有 body，RFC 7230 3.3.2 禁止这两类响应带 Content-Length
        bool has_cl = this->headers_.find("Content-Length") != nullptr;
        bool bodyless = (status_ >= 100 && status_ < 200) || status_ == 204;

        std::string out;
        // 状态行
        out += version_;
        out += ' ';
        out += std::to_string(status_);
        out += ' ';
        out += status_text_;
        out += "\r\n";

        for (auto& [k, v] : headers_) {
            out += k;
            out += ": ";
            out += v;
            out += "\r\n";
        }
        if (!has_cl && !bodyless) {
            out += "content-length: ";
            out += std::to_string(body_.size());
            out += "\r\n";
        }
        // 空行
        out += "\r\n";
        // body
        out += body_;
        return out;
    }
};
