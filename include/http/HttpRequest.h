// HTTP 请求结构
#pragma once

#include <string>

#include "HeaderMap.h"

struct HttpRequest {
    std::string method_;                             // 请求方式
    std::string path_;                               // 资源路径
    std::string version_;                            // 协议版本
    HeaderMap headers_;                              // 头名大小写不敏感 写入与查找同一口径
    std::string body_;
};
