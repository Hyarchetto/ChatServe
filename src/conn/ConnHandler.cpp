// ConnHandler — io worker 连接泵实现 业务不上本线程
#include "conn/ConnHandler.h"
#include "conn/Connection.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <utility>

#include "http/HttpResponse.h"
#include "ws/WsFrame.h"
#include "ws/WsOpcode.h"
#include "ws/WsUpgradeResponse.h"

ConnHandler::ConnHandler(EventLoop& loop, int io_index,
                         Mailbox<CtrlUp>& ctrl_inbox)
    : loop_(loop)
    , io_(io_index)
    , writer_(loop, [this](const std::shared_ptr<Connection>& c) {
          this->close_connection(c);
      })
    , outbox_(loop, [this](CtrlDown d) { this->downlink(std::move(d)); })
    , ctrl_inbox_(ctrl_inbox) {}

// ======================================== 连接管理 ========================================
void ConnHandler::add_connection(int fd) {
    // Connection 内部按 fd 与归属 io 建 Session 控制块 身份即对象 跨线程引用计数保活
    auto conn = std::make_shared<Connection>(fd, this->io_);
    this->conns_[conn->sess_.get()] = conn;
    this->loop_.add_event(fd, EPOLLIN | EPOLLET,
        [this, conn]() { this->handle_clientfd(conn); },
        [this, conn]() { this->writer_.handle_write(conn); },
        [this, conn]() { this->close_connection(conn); });
}

void ConnHandler::close_connection(const std::shared_ptr<Connection>& conn) {
    if (!conn->sess_->alive_) {
        return;
    }
    conn->sess_->alive_ = false;  // 业务侧可见该连接已死
    this->conns_.erase(conn->sess_.get());
    this->writer_.remove_pending(conn);
    this->loop_.del_event(conn->sess_->fd_);
    // 报关闭的判据就是升级成功 与中控据此建业务态是同一个事实 不可能打架
    if (conn->ws_mode_) {
        CtrlUp up;
        up.kind_ = CtrlUpKind::CLOSED;
        up.sess_ = conn->sess_;
        this->uplink(up);
    }
}

// ======================================== 读取与分流 ========================================
bool ConnHandler::pump_read(const std::shared_ptr<Connection>& conn) {
    int clientfd = conn->sess_->fd_;
    char temp_buffer[BUFFER_SIZE];

    while (true) {
        ssize_t n = recv(clientfd, temp_buffer, sizeof(temp_buffer), 0);
        if (n > 0) {
            conn->read_buf_.append(temp_buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            this->close_connection(conn);
            return false;
        }
        if (errno == EAGAIN) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        perror("recv");
        this->close_connection(conn);
        return false;
    }
    return true;
}

void ConnHandler::handle_clientfd(const std::shared_ptr<Connection>& conn) {
    if (!this->pump_read(conn) || conn->read_buf_.empty()) {
        return;
    }
    // 未升级走 HTTP 决策并施加 升级握手在施加里完成
    if (!conn->ws_mode_) {
        this->handle_http(conn, this->http_.handle(
            {conn->read_buf_.data(), conn->read_buf_.size()}));
    }
    // 已是 WS 或刚升级 同段到达的首批 WS 帧当帧处理
    if (conn->ws_mode_ && !conn->read_buf_.empty()) {
        this->handle_ws(conn, this->ws_.handle(
            {conn->read_buf_.data(), conn->read_buf_.size()}, &conn->ws_frag_));
    }
}

// 施加一条 HTTP 决策到连接 与 handle_ws 对称
// 本层是唯一同时看得见 HTTP 与 WS 的地方 升级握手在此交汇
void ConnHandler::handle_http(const std::shared_ptr<Connection>& conn,
                              HttpAction action) {
    conn->read_buf_.consume(action.consumed_);
    for (auto& wire : action.responses_) {
        this->writer_.enqueue(conn, std::move(wire), true);
    }
    if (action.close_) {
        conn->sess_->close_ = true;
    }
    if (action.upgrade_) {
        // 构造 101 响应 key 缺失时为 400 握手失败同 400 语义 只关连接
        HttpResponse resp = WsUpgradeResponse::build(action.upgrade_request_);
        conn->ws_mode_ = (resp.status_ == 101);
        if (!conn->ws_mode_) {
            conn->sess_->close_ = true;
        }
        // 先定 ws_mode_ 再出包 出包若同步失败触发的关闭才判得对要不要报 CLOSED
        this->writer_.enqueue(conn, resp.serialize(), true);
    }
}

// ======================================== 上行 ========================================
void ConnHandler::uplink(CtrlUp up) {
    this->ctrl_inbox_.post(std::move(up));
}

// 上行一批应用消息 文本或二进制分块 按入队序逐条上报
void ConnHandler::uplink_messages(const std::shared_ptr<Connection>& conn,
                                  std::vector<std::string> items, bool binary) {
    for (auto& item : items) {
        CtrlUp up;
        up.kind_ = binary ? CtrlUpKind::WS_BINARY : CtrlUpKind::WS_TEXT;
        up.sess_ = conn->sess_;
        up.text_ = std::move(item);
        this->uplink(std::move(up));
    }
}

// ======================================== WS 决策施加 ========================================
void ConnHandler::handle_ws(const std::shared_ptr<Connection>& conn,
                            WsAction action) {
    conn->read_buf_.consume(action.consumed_);
    for (auto& wire : action.responses_) {
        this->writer_.enqueue(conn, std::move(wire), true);
    }
    // 上行中控 文本消息与二进制分块各按类型上报
    this->uplink_messages(conn, std::move(action.messages_), false);
    this->uplink_messages(conn, std::move(action.binaries_), true);
    // CLOSE 已回包 标记关闭 写调度发完即回收
    if (action.close_) {
        conn->sess_->close_ = true;
    }
}

// ======================================== 下行 ========================================
void ConnHandler::downlink(CtrlDown down) {
    auto it = this->conns_.find(down.sess_.get());
    if (it == this->conns_.end() || !it->second->ws_mode_) {
        return;  // 目标已关闭或尚未升级 弃帧
    }
    auto& conn = it->second;
    // 二进制文件分块低优先 文本高优先 与旧写引擎语义一致
    if (down.binary_) {
        std::string frame = WsFrame::build(WsOpcode::BINARY, std::move(down.text_));
        this->writer_.enqueue(conn, std::move(frame), false);
    }
    else {
        std::string frame = WsFrame::build(WsOpcode::TEXT, std::move(down.text_));
        this->writer_.enqueue(conn, std::move(frame), true);
    }
}
