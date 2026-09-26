// ConnHandler — io worker 连接泵实现 业务不上本线程
#include "conn/ConnHandler.h"
#include "conn/Connection.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <utility>

#include "conn/Heartbeat.h"
#include "http/HttpResponse.h"
#include "ws/WsFrame.h"
#include "ws/WsOpcode.h"
#include "ws/WsUpgradeResponse.h"

ConnHandler::ConnHandler(EventLoop& loop, int io_index,
                         Mailbox<CtrlUp>& ctrl_uplink_box)
    : loop_(loop)
    , io_(io_index)
    , writer_(loop, [this](const std::shared_ptr<Connection>& c) {
          this->close_connection(c);
      })
    , heartbeat_(Heartbeat::kTickInterval, [this]() { this->on_tick(); })
    , downlink_box_([this](std::vector<CtrlDown>& ds) {
          this->downlink_batch(ds);
      })
    , ctrl_uplink_box_(ctrl_uplink_box) {}

// 装配到事件循环
bool ConnHandler::attach() {
    // 装配本地邮箱和心跳事件
    return this->downlink_box_.attach(this->loop_) && this->heartbeat_.attach(this->loop_);
}

// ======================================== 连接管理 ========================================
void ConnHandler::add_connection(int fd) {
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
    // 注册失败直接返回
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
    // 连接已关闭直接退出
    if (!conn->sess_->alive_) {
        return;
    }
    conn->sess_->alive_ = false;  // 业务侧可见该连接已死
    this->conns_.erase(conn->sess_.get());
    this->writer_.remove_pending(conn);
    this->loop_.del_event(conn->sess_->fd_);
    // 如果已经升级为ws模式，额外通知业务层
    if (conn->ws_mode_) {
        CtrlUp up;
        up.kind_ = CtrlUpKind::CLOSED;
        up.sess_ = conn->sess_;
        this->uplink(up);
    }
}

// ======================================== 心跳 ========================================
// 心跳节拍 取当前时刻扫一遍
void ConnHandler::on_tick() {
    this->on_tick(std::chrono::steady_clock::now());
}

// 扫描全部连接 全静默过一个节拍的发 PING 过三个节拍的判死
// 先整表快照再动作 入队与关闭都会改 conns_ 边遍历边动迭代器就失效了
void ConnHandler::on_tick(std::chrono::steady_clock::time_point now) {
    std::vector<std::shared_ptr<Connection>> snapshot;
    snapshot.reserve(this->conns_.size());
    for (auto& entry : this->conns_) {
        snapshot.push_back(entry.second);
    }

    std::chrono::milliseconds oldest{0};
    size_t closed = 0;
    for (auto& conn : snapshot) {
        const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - conn->last_activity_);
        switch (Heartbeat::judge(idle, conn->ws_mode_)) {
            case Heartbeat::Action::PING:
                // 已表达关闭意图的连接不再入队
                if (!this->writer_.is_closing(conn)) {
                    this->writer_.enqueue_probe(conn, WsFrame::build(WsOpcode::PING, ""));
                }
                break;
            case Heartbeat::Action::CLOSE:
                oldest = std::max(oldest, idle);
                this->close_connection(conn);
                ++closed;
                break;
            case Heartbeat::Action::NONE:
                break;
        }
    }
    // 汇总一行 一个节拍里可能收掉上千条 逐条打会和其他线程的输出交错
    if (closed > 0) {
        std::cerr << "心跳超时关闭 " << closed << " 条 最早空闲 " << oldest.count()
                  << "ms" << std::endl;
    }
}

// ======================================== 读取与分流 ========================================
bool ConnHandler::pump_read(const std::shared_ptr<Connection>& conn) {
    int client_fd = conn->sess_->fd_;
    char temp_buffer[kBufferSize];

    while (true) {
        ssize_t n = recv(client_fd, temp_buffer, sizeof(temp_buffer), 0);
        if (n > 0) {
            // 收到任何字节都算一次活跃 不区分是业务帧还是对 PING 的回包
            conn->last_activity_ = std::chrono::steady_clock::now();
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

// 本层是唯一同时看得见 HTTP 与 WS 的地方 升级握手在此交汇
void ConnHandler::handle_http(const std::shared_ptr<Connection>& conn, HttpAction action) {
    conn->read_buf_.consume(action.consumed_);
    for (auto& wire : action.responses_) {
        this->writer_.enqueue(conn, std::move(wire));
    }
    bool want_close = action.close_;
    if (action.upgrade_) {
        // 构造 101 响应 key 缺失时为 400 握手失败同 400 语义 只关连接
        HttpResponse resp = WsUpgradeResponse::build(action.upgrade_request_);
        conn->ws_mode_ = (resp.status_ == 101);
        want_close = want_close || !conn->ws_mode_;
        // 先定 ws_mode_ 再出包 出包若同步失败触发的关闭才判得对要不要报 CLOSED
        this->writer_.enqueue(conn, resp.serialize());
    }
    // 关闭意图在全部回包入队后统一表达 冲刷完由写引擎回调回收
    if (want_close) {
        this->writer_.request_close(conn);
    }
}

// ======================================== WS 决策施加 ========================================
void ConnHandler::handle_ws(const std::shared_ptr<Connection>& conn, WsAction action) {
    conn->read_buf_.consume(action.consumed_);
    for (auto& wire : action.responses_) {
        this->writer_.enqueue(conn, std::move(wire));
    }
    // 上行中控 一条决策里的文本与二进制合成一批
    this->uplink_ws(conn, action);
    // CLOSE 回包已入队 冲刷完由写引擎回调回收
    if (action.close_) {
        this->writer_.request_close(conn);
    }
}

// ======================================== 上行单条 ========================================
void ConnHandler::uplink(CtrlUp up) {
    this->ctrl_uplink_box_.post(std::move(up));
}

// 上行一条 WS 决策里的全部应用消息
void ConnHandler::uplink_ws(const std::shared_ptr<Connection>& conn, WsAction& action) {
    std::vector<CtrlUp> ups;
    ups.reserve(action.messages_.size() + action.binaries_.size());
    for (auto& item : action.messages_) {
        ups.push_back(CtrlUp{CtrlUpKind::WS_TEXT, conn->sess_, std::move(item)});
    }
    for (auto& item : action.binaries_) {
        ups.push_back(CtrlUp{CtrlUpKind::WS_BINARY, conn->sess_, std::move(item)});
    }
    this->ctrl_uplink_box_.post_batch(std::move(ups));
}

// ======================================== 下行 ========================================
// 整批一次处理 按目标连接逐条组帧投给写调度
void ConnHandler::downlink_batch(std::vector<CtrlDown>& downs) {
    for (auto& down : downs) {
        auto it = this->conns_.find(down.sess_.get());
        if (it == this->conns_.end()) {
            continue;  // 目标已关闭 弃帧
        }
        // 载荷类型到帧类型的映射 方向由 ctrl 层指向 ws 层
        const WsOpcode opcode = (down.kind_ == CtrlDownKind::WS_BINARY)
                                    ? WsOpcode::BINARY
                                    : WsOpcode::TEXT;
        this->writer_.enqueue(it->second, WsFrame::build(opcode, std::move(down.text_)));
    }
}
