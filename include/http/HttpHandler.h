// HttpHandler — HTTP 协议决策器
// 吃缓冲区字节，吐一条决策，解析与路由都在本层
// WebSocket 升级只标记检出，101 响应构造由施加侧做
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "HttpRequest.h"
#include "HttpRouter.h"

// 一条 HTTP 处理决策，由施加侧应用到连接
struct HttpAction {
    std::vector<std::string> responses_;  // 按序待发线路数据
    HttpRequest upgrade_request_;         // upgrade_ 为真时有效，交给施加侧握手
    size_t consumed_ = 0;                 // 累计已消耗字节，施加侧一次性 consume
    bool close_ = false;                  // 关闭意图，写引擎冲刷后执行
    bool upgrade_ = false;                // 检出 WebSocket 升级请求
};

class HttpHandler {
public:
    // 解析并路由缓冲区，返回待施加决策
    HttpAction handle(std::string_view buf);

private:
    // 路由只被本类消费，路由表构造时固定，运行期只读
    HttpRouter http_router_;
};
