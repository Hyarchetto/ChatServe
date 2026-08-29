// HttpHandler — HTTP 协议处理器
#include "http/HttpHandler.h"
#include "conn/Connection.h"
#include "core/EventLoop.h"
#include "core/ThreadPool.h"
#include "http/HttpParser.h"
#include "http/ErrorResponse.h"
#include "http/HttpRouter.h"
#include "ws/WsUpgradeResponse.h"
#include "conn/WriteScheduler.h"

HttpHandler::HttpHandler(EventLoop& loop, ThreadPool& works,
                        WriteScheduler& writer)
    : loop_(loop), works_(works), writer_(writer) {}

void HttpHandler::handle_http(const std::shared_ptr<Connection>& conn) {
    while (true) {
        HttpResult result = HttpParser::handle({conn->read_buf_.data(), conn->read_buf_.size()});

        switch (result.type_) {
            // 未完成直接结束
            case HttpResultType::INCOMPLETE: {
                return;
            }
            // 错误的 HTTP 请求可能是网络问题或者网络攻击，直接断开好了
            case HttpResultType::BAD_REQUEST: {
                auto resp = ErrorResponse::bad_request(result.error_msg_);
                std::string wire = resp.serialize();
                conn->pending_close_ = true;
                this->writer_.push_response(conn, std::move(wire), true);
                this->writer_.flush_responses();
                return;
            }
            // 处理 WebSocket 协议升级请求
            case HttpResultType::WS_UPGRADE: {
                auto resp = WsUpgradeResponse::build(result.request_);
                std::string wire = resp.serialize();
                // 判断响应情况
                if (resp.status_ == 101) {
                    conn->read_buf_.consume(result.finished_);
                    conn->ws_mode_ = true;
                }
                else{
                    conn->pending_close_ = true;
                }
                // 升级成功切换到 WS 模式后交给 WriteScheduler 发送
                this->writer_.push_response(conn, std::move(wire), true);
                this->writer_.flush_responses();
                return;
            }
            // 处理普通的 HTTP 协议
            case HttpResultType::OK: {
                conn->read_buf_.consume(result.finished_);
                // 提交给线程池解析，生成响应
                this->works_.submit([this, conn, req = std::move(result.request_)]() {
                    HttpResponse resp = this->http_router_.handle(req);
                    std::string wire = resp.serialize();
                    // 获取响应再线程池将后续的响应工作交给事件循环
                    this->loop_.run_in_loop([this, conn, wire = std::move(wire)]() {
                        this->writer_.push_response(conn, std::move(wire), true);
                        this->writer_.flush_responses();
                    });
                });
                break;
            }
        }
    }
}
