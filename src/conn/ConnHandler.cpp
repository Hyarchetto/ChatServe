// ConnHandler — io worker 连接泵实现 业务不上本线程
#include "conn/ConnHandler.h"
#include "conn/Connection.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <iostream>
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
    // 构造失败 fd 尚未交给 Connection，由这里关闭
    std::shared_ptr<Connection> conn;
    try {
        conn = std::make_shared<Connection>(fd, this->io_);
    }
    catch (const std::exception& e) {
        std::cerr << "建立连接失败 fd=" << fd << " " << e.what() << std::endl;
        close(fd);
        return;
    }
    // 挂不上监听就不会有任何事件到达，连接无意义直接收
    // 此时尚未登记进 conns_，Connection 析构关 fd
    if (!this->loop_.add_event(fd, EPOLLIN | EPOLLET,
            [this, conn]() { this->handle_client_fd(conn); },
            [this, conn]() { this->writer_.handle_write(conn); },
            [this, conn]() { this->close_connection(conn); })) {
        std::cerr << "注册连接事件失败 fd=" << fd << std::endl;
        return;
    }
    this->conns_[conn->sess_.get()] = conn;
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
    int client_fd = conn->sess_->fd_;
    char temp_buffer[kBufferSize];

    while (true) {
        ssize_t n = recv(client_fd, temp_buffer, sizeof(temp_buffer), 0);
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

void ConnHandler::handle_client_fd(const std::shared_ptr<Connection>& conn) {
    if (!this->pump_read(conn) || conn->read_buf_.empty()) {
        return;
    }
    // 未升级走 HTTP 决策并施加 升级握手在施加里完成
    if (!conn->ws_mode_) {
        this->handle_http(conn, this->http_.handle({conn->read_buf_.data(),
                                                   conn->read_buf_.size()}));
    }
    // 已是 WS 或刚升级 同段到达的首批 WS 帧当帧处理
    if (conn->ws_mode_ && !conn->read_buf_.empty()) {
        this->handle_ws(conn, this->ws_.handle({conn->read_buf_.data(), conn->read_buf_.size()},
                                               &conn->ws_frag_));
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
    bool want_close = action.close_;
    if (action.upgrade_) {
        // 构造 101 响应 key 缺失时为 400 握手失败同 400 语义 只关连接
        HttpResponse resp = WsUpgradeResponse::build(action.upgrade_request_);
        conn->ws_mode_ = (resp.status_ == 101);
        want_close = want_close || !conn->ws_mode_;
        // 先定 ws_mode_ 再出包 出包若同步失败触发的关闭才判得对要不要报 CLOSED
        this->writer_.enqueue(conn, resp.serialize(), true);
    }
    // 关闭意图在全部回包入队后统一表达 冲刷完由写引擎回调回收
    if (want_close) {
        this->writer_.request_close(conn);
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
    // CLOSE 回包已入队 冲刷完由写引擎回调回收
    if (action.close_) {
        this->writer_.request_close(conn);
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

// ======================================== 下行 ========================================
void ConnHandler::downlink(CtrlDown down) {
    auto it = this->conns_.find(down.sess_.get());
    if (it == this->conns_.end()) {
        return;  // 目标已关闭 弃帧
    }
    auto& conn = it->second;
    // 二进制文件分块低优先 文本高优先
    if (down.binary_) {
        std::string frame = WsFrame::build(WsOpcode::BINARY, std::move(down.text_));
        this->writer_.enqueue(conn, std::move(frame), false);
    }
    else {
        std::string frame = WsFrame::build(WsOpcode::TEXT, std::move(down.text_));
        this->writer_.enqueue(conn, std::move(frame), true);
    }
}
