// WebSocket 应用层协议解析器 — 解析/构建自定义文本协议
// 协议格式：COMMAND|param1|param2|...
// 协议：JOIN|房间|昵称  →  OK|房间|昵称 / MSG|昵称|内容 / SYS|消息 / MEMBERS|列表
// 聊天统一走 MSG 命令，内容取 MSG| 之后的剩余部分，内容可含 |
// 有 | 分隔的第一段为命令字，其余为参数；无 | 的裸文本由默认处理器兜底
#pragma once

#include <string>
#include <vector>

#include "WsAppMessage.h"

class WsAppParser {
public:
    static WsAppMessage parse(const std::string& data);

    static std::string build(const std::string& command,
                             const std::vector<std::string>& params);
    // 定长参数经变参模板转成 vector 复用同一实现 避免手写多个重载
    template <typename... Args>
    static std::string build(const std::string& command, const Args&... params) {
        return build(command, std::vector<std::string>{params...});
    }

    static constexpr char DELIMITER = '|';
};
