// HTTP 错误种类 — 每一种对应一张固定的错误页面
#pragma once

enum class HttpError {
    NONE,                   // 无错误，默认值
    REQUEST_LINE_SYNTAX,    // 请求行三截不全
    METHOD,                 // 只支持 GET 与 POST
    HEADER_SYNTAX,          // 冒号缺失或头名为空
    DUPLICATE_LENGTH,       // 重复的 Content-Length
    TRANSFER_ENCODING,      // 请求头带了 Transfer-Encoding
    INVALID_LENGTH,         // Content-Length 不是十进制数字
    HEADER_TOO_LARGE,       // 请求行与请求头累计越过上限
    BODY_TOO_LARGE,         // Content-Length 越过上限
    MISSING_WS_KEY,         // 升级请求缺 Sec-WebSocket-Key
    FILE_TOO_LARGE,         // 静态文件超过单文件上限
    READ_FAILED,            // 静态文件读取失败
};
