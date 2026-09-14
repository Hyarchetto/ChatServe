// WebSocket 应用层消息 — 解析后的结构化表示
#pragma once

#include <string>
#include <vector>

struct AppMessage {
    std::string command_;                // 自定义协议头
    std::vector<std::string> params_;    // 参数列表
    std::string rest_;                   // 内容原文

    size_t param_count() const {
        return params_.size();
    }
    const std::string& param(size_t i) const {
        return params_[i];
    }
};
