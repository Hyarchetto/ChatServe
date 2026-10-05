// HTTP 请求解析器 — 无状态，进度由调用方按连接保管在 HttpRequestState 上
#pragma once

#include <string>
#include <string_view>

#include "./HttpError.h"
#include "./HttpRequest.h"
#include "./HttpRequestState.h"

// 解析结果类型
enum class HttpResultType {
    INCOMPLETE,                         // 数据不完整，需要继续接收
    BAD_REQUEST,                        // 格式错误
    WS_UPGRADE,                         // WebSocket 升级请求
    OK                                  // 普通 HTTP 请求
};

// 解析结果
struct HttpResult {
    HttpResultType type_ = HttpResultType::INCOMPLETE;
    HttpRequest    request_;                        // 完成时成形，由 state 移出
    HttpError      error_ = HttpError::NONE;        // BAD_REQUEST 时指出是哪一种
    size_t         consumed_ = 0;                   // 本次调用已消耗的字节数，不完整时也可能非零
    bool           close_ = false;                  // 客户端要求关闭连接
};

// HTTP 解析器
class HttpParser {
public:
    // 请求行与请求头的累计上限取16KB，防客户端慢滴头部撑爆读缓冲
    static constexpr size_t kMaxHeaderBytes = 16 * 1024;

    HttpParser() = delete;
    // 喂数据并按 st 的进度续解析，完成时 request_ 移出，st 复位待下一条请求
    static HttpResult handle(std::string_view buf, HttpRequestState& st);
private:
    // 从 pos 读一行，返回 true 表示读到了完整行
    static bool read_line(std::string_view buf, size_t& pos, std::string_view& line);
};
