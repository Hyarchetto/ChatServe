// HttpHandler — HTTP 协议决策器 纯函数实现
#include "http/HttpHandler.h"

#include <utility>

#include "http/HttpParser.h"
#include "http/HttpResponse.h"
#include "http/ErrorResponse.h"

HttpAction HttpHandler::handle(std::string_view buf) {
    HttpAction action;
    while (true) {
        // 从已消耗位置续解析 支持同段到达的多个请求
        HttpResult result = HttpParser::handle(buf.substr(action.consumed_));
        action.consumed_ += result.consumed_;

        switch (result.type_) {
            // 未完成直接结束
            case HttpResultType::INCOMPLETE: {
                return action;
            }
            // 错误的 HTTP 请求可能是网络问题或者网络攻击 直接断开好了
            case HttpResultType::BAD_REQUEST: {
                action.responses_.push_back(
                    ErrorResponse::build_bad_request(result.error_msg_).serialize());
                action.close_ = true;
                return action;
            }
            // 检出 WebSocket 升级请求 101 响应构造与模式切换交给施加侧
            case HttpResultType::WS_UPGRADE: {
                action.upgrade_ = true;
                action.upgrade_request_ = std::move(result.request_);
                return action;
            }
            // 普通的 HTTP 请求 内联路由
            case HttpResultType::OK: {
                action.responses_.push_back(
                    this->http_router_.handle(result.request_).serialize());
                break;
            }
        }
    }
}
