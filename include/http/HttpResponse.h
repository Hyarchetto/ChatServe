// HTTP 响应结构，serialize() 生成线路数据
#pragma once

#include <string>
#include <unordered_map>
#include <sstream>

struct HttpResponse {
    int status_ = 200;
    std::string status_text_ = "OK";
    std::string version_ = "HTTP/1.1";
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;

    // 标准序列化：状态行 + 头部 + 空行 + body
    std::string serialize() const {
        std::ostringstream oss;
        // 状态行
        oss << version_ << ' ' << status_ << ' ' << status_text_ << "\r\n";

        // 如果调用方未显式设置 Content-Length，自动计算
        bool has_cl = headers_.find("Content-Length") != headers_.end();
        for (auto& [k, v] : headers_) {
            oss << k << ": " << v << "\r\n";
        }
        if (!has_cl) {
            oss << "Content-Length: " << body_.size() << "\r\n";
        }
        // 空行
        oss << "\r\n";
        // body
        oss << body_;
        return oss.str();
    }
};
