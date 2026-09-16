// HTTP 请求结构
#pragma once

#include <string>
#include <optional>

#include "HeaderMap.h"

struct HttpRequest {
    std::string method_;                             // 请求方式
    std::string path_;                               // 资源路径
    std::string version_;                            // 协议版本
    HeaderMap headers_;                              // 键均已归一化为小写
    std::string body_;

    // 按 key 大小写不敏感查找 不存在返回 nullopt
    std::optional<std::string> find_header(const std::string& key) const {
        auto it = headers_.find(lowercase(key));
        if (it == headers_.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};
