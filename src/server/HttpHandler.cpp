// HttpHandler — HTTP 协议处理器
#include "server/HttpHandler.h"
#include "server/Connection.h"
#include "core/EventLoop.h"
#include "core/ThreadPool.h"
#include "http/HttpParser.h"
#include "http/ErrorResponse.h"
#include "http/HttpRouter.h"
#include "ws/WsUpgradeResponse.h"
#include "server/WriteScheduler.h"

#include <sys/socket.h>
#include <unistd.h>

HttpHandler::HttpHandler(EventLoop& loop, ThreadPool& works,
                        HttpRouter& http_router, WriteScheduler& writer,
                        CloseConnectionFn close_conn)
    : loop_(loop), works_(works), http_router_(http_router), writer_(writer),
      close_connection_(std::move(close_conn)) {}

void HttpHandler::handle_http(const std::shared_ptr<Connection>& conn) {
    int clientfd = conn->fd_;

    while (true) {
        HttpResult result = HttpParser::handle(conn->read_buf_);

        switch (result.type_) {
            // 未完成直接结束
            case HttpResultType::INCOMPLETE: {
                return;
            }
            // 错误的 HTTP 请求可能是网络问题或者网络攻击，直接断开好了
            case HttpResultType::BAD_REQUEST: {
                auto resp = ErrorResponse::bad_request(result.error_msg_);
                std::string wire = resp.serialize();
                send(clientfd, wire.data(), wire.size(), 0);
                this->close_connection_(conn);
                return;
            }
            // 处理 WebSocket 协议升级请求
            case HttpResultType::WS_UPGRADE: {
                auto resp = WsUpgradeResponse::build(result.request_);
                std::string wire = resp.serialize();
                // 升级失败直接断开连接
                if (resp.status_ != 101) {
                    send(clientfd, wire.data(), wire.size(), 0);
                    this->close_connection_(conn);
                    return;
                }
                // 升级成功切换到 WS 模式后交给 WriteScheduler 发送
                conn->read_buf_.erase(0, result.finished_);
                conn->ws_mode_ = true;
                this->writer_.push_response(conn, std::move(wire), true);
                this->writer_.flush_responses();
                return;
            }
            // 处理普通的 HTTP 协议
            case HttpResultType::OK: {
                conn->read_buf_.erase(0, result.finished_);
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
