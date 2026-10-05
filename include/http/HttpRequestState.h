// HTTP 请求解析的跨调用状态
// 一次请求分多次到达时，已读齐的部分在此累积
#pragma once

#include <cstddef>

#include "HttpRequest.h"

// 请求解析的阶段
enum class HttpPhase { 
    REQUEST_LINE, 
    HEADERS, 
    BODY 
};

struct HttpRequestState {
    HttpPhase   phase_ = HttpPhase::REQUEST_LINE;   // 当前读到了哪一段
    HttpRequest request_;                           // 已读齐的方法、路径、版本、头、体
    size_t      header_bytes_ = 0;                  // 已消费掉的请求行与头部字节数
    size_t      content_length_ = 0;                // 头部里的 Content-Length
};
