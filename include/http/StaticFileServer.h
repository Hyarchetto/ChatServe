#pragma once

#include <string>

#include "./HttpResponse.h"

// 静态文件服务 — 从磁盘读取文件并返回 HttpResponse
// 错误页面的生成和重定向不在这里，各模块自己做
class StaticFileServer {
public:
    // 单文件读取上限，一次读进内存会占着 io 线程，超过直接拒绝
    static constexpr size_t kMaxFileSize = 64 * 1024 * 1024;

    // 把请求路径解析为该服务目录下的真实路径
    // 做百分号解码并拒绝越出目录的路径，非法或解析失败返回空串
    static std::string resolve(const std::string& request_path);

    // 读取文件并返回 HttpResponse
    // 文件不存在或不是普通文件返回 404，超过单文件上限返回 413
    static HttpResponse serve(const std::string& file_path);
};
