// HTTP 请求结构
#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <cctype>
#include <algorithm>

struct HttpRequest {
    std::string method_;                             // 请求方式
    std::string path_;                               // 资源路径
    std::string version_;                            // 协议版本
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;

    // ASCII 大小写不敏感相等 逐字符转 unsigned char 免得负值喂给 tolower
    static bool ieq(std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char ca, char cb) {
                   return std::tolower(static_cast<unsigned char>(ca)) ==
                          std::tolower(static_cast<unsigned char>(cb));
               });
    }

    // 在头部表里按 key 大小写不敏感查找 找不到返回 nullptr
    static const std::string* find_header_ci(
            const std::unordered_map<std::string, std::string>& headers,
            const std::string& key) {
        auto it = headers.find(key);
        if (it != headers.end()) {
            return &it->second;
        }
        for (auto& [k, v] : headers) {
            if (ieq(k, key)) {
                return &v;
            }
        }
        return nullptr;
    }

    // 按 key 大小写不敏感查找 不存在返回 nullopt
    std::optional<std::string> find_header(const std::string& key) const {
        if (const std::string* v = find_header_ci(headers_, key)) {
            return *v;
        }
        return std::nullopt;
    }
};
