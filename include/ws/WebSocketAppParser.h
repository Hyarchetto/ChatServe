// WebSocket 应用层协议解析器 — 解析/构建自定义文本协议
// 协议格式：COMMAND|param1|param2|...
// 协议：JOIN|房间|昵称  →  OK|房间|昵称 / MSG|昵称|内容 / SYS|消息 / MEMBERS|列表
// 有 | 分隔的第一段为命令字，其余为参数；无 | 的文本视为裸消息（如聊天内容）
#pragma once

#include <string>
#include <vector>

#include "WebSocketAppMessage.h"

class WebSocketAppParser {
public:
    static WebSocketAppMessage parse(const std::string& data);

    static std::string build(const std::string& command,
                             const std::vector<std::string>& params);
    static std::string build(const std::string& command,
                             const std::string& param1);
    static std::string build(const std::string& command,
                             const std::string& param1,
                             const std::string& param2);

    static constexpr char DELIMITER = '|';
};
