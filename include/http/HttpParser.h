// HTTP 请求解析器 — 每次从头解析，不做增量状态追踪
// 本文件只做一件事：从 TCP buffer 中解析出 HTTP 请求
#pragma once

#include <string>
#include <sstream>
#include <cstring>
#include <algorithm>

#include "./HttpRequest.h"

// 解析结果类型
enum class HttpResultType {
    INCOMPLETE,    // 数据不完整，需要继续接收
    BAD_REQUEST,   // 格式错误
    WS_UPGRADE,    // WebSocket 升级请求
    OK,            // 普通 HTTP 请求
};

// 解析结果
struct HttpResult {
    HttpResultType type_ = HttpResultType::INCOMPLETE;
    HttpRequest  request_;
    std::string  error_msg_;           // BAD_REQUEST 时描述错误原因
    size_t       finished_ = 0;        // 已消耗的字节数
};

// HTTP 解析器
class HttpParser {
public:
    HttpParser() = default;
    // 喂数据并解析，返回解析结果
    static HttpResult handle(const std::string& buf);
private:
    // 从 pos 读一行，返回 true 表示读到了完整行
    static bool read_line(const std::string& buf, size_t& pos, std::string& line);
};
