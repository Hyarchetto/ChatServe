// WebSocket 应用层消息 — 解析后的结构化表示
#pragma once

#include <string>
#include <vector>
#include <unordered_set>

struct WebSocketAppMessage {
    std::string command_;                // 命令字（JOIN/MSG/SYS 等），裸文本消息为空
    std::vector<std::string> params_;    // | 分隔的参数列表
    std::string raw_;                    // 原始消息原文

    bool is_command() const {
        return !command_.empty();
    }
    size_t param_count() const {
        return params_.size();
    }
    const std::string& param(size_t i) const {
        return params_[i];
    }
};
