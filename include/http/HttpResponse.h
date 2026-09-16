// HTTP 响应结构，serialize() 生成线路数据
#pragma once

#include <string>
#include <sstream>

#include "HeaderMap.h"

struct HttpResponse {
    int status_ = 200;
    std::string status_text_ = "OK";
    std::string version_ = "HTTP/1.1";
    HeaderMap headers_;                              // 键均已归一化为小写
    std::string body_;

    // 标准序列化：状态行 + 头部 + 空行 + body
    std::string serialize() const {
        std::ostringstream oss;
        // 状态行
        oss << version_ << ' ' << status_ << ' ' << status_text_ << "\r\n";

        // 调用方未显式设置 Content-Length 时才自动计算
        bool has_cl = this->has_header("Content-Length");
        for (auto& [k, v] : headers_) {
            oss << k << ": " << v << "\r\n";
        }
        if (!has_cl) {
            oss << "content-length: " << body_.size() << "\r\n";
        }
        // 空行
        oss << "\r\n";
        // body
        oss << body_;
        return oss.str();
    }

    // 头部名大小写不敏感查找，大小写不同的同名字段算同一个
    bool has_header(const std::string& key) const {
        return headers_.find(lowercase(key)) != headers_.end();
    }
};
