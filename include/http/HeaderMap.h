// HTTP 头表 — 头名大小写不敏感
// 键入表前一律经 lowercase() 归一化成小写，查找走同样口径，查表就是普通哈希
// 归一化只此一处，存储与查找共用，免得两边口径漂移
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

// 键一律小写，写入方负责先过 lowercase()
using HeaderMap = std::unordered_map<std::string, std::string>;
