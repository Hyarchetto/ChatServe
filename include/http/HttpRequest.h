// HTTP 请求结构
#pragma once

#include <string>
#include <unordered_map>
#include <cctype>
#include <algorithm>

struct HttpRequest {
    std::string method_;                             // 请求方式
    std::string path_;                               // 资源路径
    std::string version_;                            // 协议版本
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;

    // 取 header 值，不存在返回空串
    std::string header(const std::string& key) const {
        auto it = headers_.find(key);
        if (it != headers_.end()) return it->second;
        // 大小写容错
        for (auto& [k, v] : headers_) {
            if (k.size() == key.size() &&
                    std::equal(k.begin(), k.end(), key.begin(),
                        [](char a, char b) { 
                            return std::tolower(a) == std::tolower(b); 
                        })) {
                return v;
            }
        }
        return {};
    }
};
