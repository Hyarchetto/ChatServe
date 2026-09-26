// HTTP 头表 — 头名大小写不敏感
// 归一化收在本类内 写入与查找共用同一口径 调用方不碰键的形态
// 遍历给出的是归一化后的键
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <cctype>
#include <algorithm>

// ASCII 转小写 头名归一化的唯一实现
inline std::string lowercase(std::string_view raw) {
    std::string out(raw);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return out;
}

// 仿 STL 容器的接口 查找与遍历按原样命名
class HeaderMap {
public:
    using const_iterator = std::unordered_map<std::string, std::string>::const_iterator;

    // 写入一条头 键归一化为小写 同名已存在则覆盖
    // 返回是否新增 调用方据此识别重复头
    bool set(std::string_view key, std::string_view value) {
        std::string normalized = lowercase(key);
        auto [it, inserted] = this->entries_.try_emplace(std::move(normalized), value);
        if (!inserted) {
            it->second.assign(value);
        }
        return inserted;
    }

    // 大小写不敏感查找 不存在返回 nullptr
    const std::string* find(std::string_view key) const {
        auto it = this->entries_.find(lowercase(key));
        return it == this->entries_.end() ? nullptr : &it->second;
    }

    size_t size() const { return this->entries_.size(); }
    const_iterator begin() const { return this->entries_.begin(); }
    const_iterator end() const { return this->entries_.end(); }

private:
    std::unordered_map<std::string, std::string> entries_;
};
