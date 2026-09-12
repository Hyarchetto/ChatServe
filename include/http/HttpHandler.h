// HttpHandler — HTTP 协议决策器 纯函数不知 io 也不知连接
// 吃缓冲区字节 吐一条决策 施加到连接的动作由 io 层完成
// WebSocket 升级只标记检出 101 响应构造与模式切换交给施加侧
// 解析与路由在本层 读写连接与 socket 不在本层 本层无任何外部依赖
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "HttpRouter.h"

// 一条 HTTP 处理决策 由施加侧应用到连接
struct HttpAction {
    std::vector<std::string> responses_;  // 按序待发线路数据
    HttpRequest upgrade_request_;         // upgrade_ 为真时有效 交给施加侧握手
    size_t consumed_ = 0;                 // 累计已消耗字节 施加侧一次性 consume
    bool close_ = false;                  // 关闭意图 写引擎冲刷后执行
    bool upgrade_ = false;                // 检出 WebSocket 升级请求
};

class HttpHandler {
public:
    // 解析并路由缓冲区 返回待施加决策 不碰连接不碰 socket
    HttpAction handle(std::string_view buf);

private:
    // 路由只被本类消费 路由表构造时固定 运行期只读
    HttpRouter http_router_;
};
