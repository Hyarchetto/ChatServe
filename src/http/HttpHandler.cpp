// HttpHandler — HTTP 协议决策器实现
#include "http/HttpHandler.h"

#include <string_view>
#include <utility>

#include "http/HttpParser.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/ErrorResponse.h"

HttpAction HttpHandler::handle(std::string_view buf, HttpRequestState& st) const {
    HttpAction action;
    while (true) {
        // 从已消耗位置续解析，支持同段到达的多个请求
        HttpResult result = HttpParser::handle(buf.substr(action.consumed_), st);
        action.consumed_ += result.consumed_;

        switch (result.type_) {
            // 未完成直接结束
            case HttpResultType::INCOMPLETE: {
                return action;
            }
            // 错误的 HTTP 请求可能是网络问题或者网络攻击，直接断开好了
            case HttpResultType::BAD_REQUEST: {
                HttpResponse resp = ErrorResponse::build(result.error_);
                // 回包冲刷完即断，把关闭意图写进响应头
                resp.headers_.set("connection", "close");
                action.responses_.push_back(resp.serialize());
                action.close_ = true;
                return action;
            }
            // 检出 WebSocket 升级请求
            case HttpResultType::WS_UPGRADE: {
                action.upgrade_ = true;
                action.upgrade_request_ = std::move(result.request_);
                return action;
            }
            // 普通的 HTTP 请求，内联路由
            case HttpResultType::OK: {
                action.responses_.push_back( this->http_router_.handle(std::move(result.request_)).serialize());
                // 客户端要求关闭则回完这一条就断，后续请求不再处理
                if (result.close_) {
                    action.close_ = true;
                    return action;
                }
                break;
            }
        }
    }
}
