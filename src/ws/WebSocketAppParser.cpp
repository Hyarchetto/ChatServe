// WebSocket 应用层协议解析器
#include "ws/WebSocketAppParser.h"

WebSocketAppMessage WebSocketAppParser::parse(const std::string& data) {
    WebSocketAppMessage msg;
    msg.raw_ = data;

    auto pipe = data.find(DELIMITER);
    if (pipe == std::string::npos) {
        return msg;  // 裸消息
    }

    // 宽松判断：只要有 '|' 就认为是命令
    msg.command_ = data.substr(0, pipe);

    size_t start = pipe + 1;
    while (start < data.size()) {
        auto next = data.find(DELIMITER, start);
        if (next == std::string::npos) {
            msg.params_.push_back(data.substr(start));
            break;
        }
        msg.params_.push_back(data.substr(start, next - start));
        start = next + 1;
    }

    return msg;
}

std::string WebSocketAppParser::build(const std::string& command,
                                      const std::vector<std::string>& params) {
    std::string result = command;
    for (const auto& p : params) {
        result += DELIMITER;
        result += p;
    }
    return result;
}

std::string WebSocketAppParser::build(const std::string& command,
                                      const std::string& param1) {
    return command + DELIMITER + param1;
}

std::string WebSocketAppParser::build(const std::string& command,
                                       const std::string& param1,
                                       const std::string& param2) {
    return command + DELIMITER + param1 + DELIMITER + param2;
}

std::string WebSocketAppParser::build(const std::string& command,
                                       const std::string& param1,
                                       const std::string& param2,
                                       const std::string& param3) {
    return command + DELIMITER + param1 + DELIMITER + param2 + DELIMITER + param3;
}

std::string WebSocketAppParser::build(const std::string& command,
                                       const std::string& param1,
                                       const std::string& param2,
                                       const std::string& param3,
                                       const std::string& param4) {
    return command + DELIMITER + param1 + DELIMITER + param2 + DELIMITER + param3 + DELIMITER + param4;
}
