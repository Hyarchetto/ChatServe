// WebSocket 应用层协议解析器
#include "app/AppParser.h"

AppMessage AppParser::parse(const std::string& data) {
    AppMessage msg;

    auto pipe = data.find(kDelimiter);
    if (pipe == std::string::npos) {
        return msg;  // 裸消息
    }

    // 有 '|' 认为是命令
    msg.command_ = data.substr(0, pipe);
    msg.rest_ = data.substr(pipe + 1);

    size_t start = pipe + 1;
    while (start < data.size()) {
        auto next = data.find(kDelimiter, start);
        if (next == std::string::npos) {
            msg.params_.push_back(data.substr(start));
            break;
        }
        msg.params_.push_back(data.substr(start, next - start));
        start = next + 1;
    }

    return msg;
}

std::string AppParser::build_frame(const std::string& command,
                                   const std::vector<std::string>& params) {
    // 帧长一次算清 省掉逐段 += 的几次重分配
    size_t total = command.size();
    for (const auto& p : params) {
        total += 1 + p.size();
    }
    std::string result;
    result.reserve(total);
    result = command;
    for (const auto& p : params) {
        result += kDelimiter;
        result += p;
    }
    return result;
}
