#pragma once

#include <string>
#include <fstream>
#include <sstream>

#include "./HttpResponse.h"

// 静态文件服务 — 从磁盘读取文件并返回 HttpResponse
// 错误页面的生成和重定向不在这里，各模块自己做
class StaticFileServer {
public:
    // 判断文件是否存在
    static bool exists(const std::string& file_path);

    // 读取文件并返回 HttpResponse，文件不存在返回 404
    static HttpResponse serve(const std::string& file_path);
};
