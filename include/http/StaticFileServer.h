#pragma once

#include <string>
#include <fstream>
#include <sstream>

#include "./HttpResponse.h"

// 静态文件服务 — 根据路径读取文件并填充 HttpResponse
class StaticFileServer {
public:
    // 判断文件是否存在
    static bool exists(const std::string& file_path);

    // 读取文件并返回 HttpResponse，文件不存在返回 404
    static HttpResponse serve(const std::string& file_path);

    // 错误页面
    static HttpResponse not_found(const std::string& path = "");
    static HttpResponse bad_request(const std::string& msg);
    static HttpResponse server_error(const std::string& msg);
    static HttpResponse method_not_allowed(const std::string& msg);

    // 重定向
    static HttpResponse redirect(const std::string& location);
};
