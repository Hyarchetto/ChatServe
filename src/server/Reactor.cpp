// 单 Reactor 多线程 TCP 服务器
// 所有连接在同一个 EventLoop 里管
// HTTP 解析后丢线程池路由，IO 线程只管收发
// WebSocket 帧解析后也丢线程池处理，结果用 run_in_loop 投回 IO 线程发
// TEXT 和 BINARY 在同一个线程池任务中顺序处理，避免 CHUNK 和数据的竞态
// 发送队列分优先级：TEXT 控制帧优先于 BINARY 文件数据
#include "server/Reactor.h"
#include "http/HttpParser.h"
#include "http/ErrorResponse.h"
#include "ws/WebSocketParser.h"
#include "ws/WebSocketFrame.h"
#include "ws/WebSocketUpgradeResponse.h"
#include "ws/WebSocketAppParser.h"

#include <iostream>
#include <cstdio>

// ======================================== 构造/析构 ========================================

Reactor::Reactor() {}

Reactor::~Reactor() {
    this->stop();
}

bool Reactor::init() {
    return this->loop_.init();
}

void Reactor::stop() {
    this->loop_.quit();
    this->works_.shutdown();
}

// ======================================== 监听 ========================================

void Reactor::start_listen(int port) {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket error");
        return;
    }

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind error");
        close(listenfd);
        return;
    }
    if (listen(listenfd, 1024) < 0) {
        perror("listen error");
        close(listenfd);
        return;
    }

    int flags = fcntl(listenfd, F_GETFL, 0);
    fcntl(listenfd, F_SETFL, flags | O_NONBLOCK);

    this->loop_.add_event(listenfd,
                          EPOLLIN | EPOLLET,
                          [this, listenfd]() { this->accept_connections(listenfd);});

    std::cout << "服务器开始监听 " << port << std::endl;
    std::cout << "------------------------------------------" << std::endl;
}

void Reactor::accept_connections(int listenfd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (true) {
        // 错误处理
        if (int clientfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_len); 
                clientfd < 0) {

            if (errno == EAGAIN) {
                break;
            }
            else if (errno == EINTR) {
                continue;
            }
            else{
                perror("accept error");
                break;
            }
        }
        // 正常情况
        else{
            this->add_connection(clientfd);
        }
    }
}

void Reactor::loop() {
    this->loop_.loop();
}

// ======================================== 连接管理 ========================================

void Reactor::add_connection(int fd) {
    auto conn = std::make_shared<Connection>(fd);
    this->connections_[fd] = conn;
    this->conn_registry_.add(fd, conn);
    this->loop_.add_event(fd, EPOLLIN | EPOLLET,
        [this, conn]() { this->handle_clientfd(conn); },
        [this, conn]() { this->handle_write(conn); },
        [this, conn]() { this->disconnect_connection(conn); });
}

void Reactor::del_connection(const std::shared_ptr<Connection>& conn) {
    int fd = conn->fd_;
    conn->alive_ = false;
    this->conn_registry_.remove(fd);
    this->pending_writes_.erase(fd);
    this->connections_.erase(fd);
    this->loop_.del_event(fd);
}

// ======================================== 响应发送 ========================================

void Reactor::push_response(const std::shared_ptr<Connection>& conn, std::string data, bool is_high_priority) {
    // 根据信息类型，放入不同优先度的响应队列
    if (is_high_priority) {
        this->queue_high_.emplace(PendingResponse{conn, std::move(data)});
    } 
    else {
        this->queue_low_.emplace(PendingResponse{conn, std::move(data)});
    }
}

void Reactor::drain_one(std::queue<PendingResponse>& q,
                        std::function<void(const std::shared_ptr<Connection>&)> del_connection) {

    std::queue<Reactor::PendingResponse> local{};
    std::swap(local, q);

    // 跟踪因 EAGAIN 未发完的 fd，跳过后续同 fd 消息避免帧交织
    std::unordered_set<int> pending_fds;

    while (!local.empty()) {
        auto item = std::move(local.front());
        local.pop();

        int fd = item.conn_->fd_;
        if (!this->loop_.has_event(fd)) {
            continue;
        }

        // 该 fd 上还有未发完数据，将消息放回队列等 EPOLLOUT 恢复后再发
        if (pending_fds.count(fd)) {
            q.push(std::move(item));
            continue;
        }

        std::string& wire = item.data_;
        ssize_t total = wire.size();
        ssize_t sent = 0;

        while (sent < total) {
            if (ssize_t n = send(fd, wire.data() + sent, total - sent, 0);
                        n > 0) {
                sent += n;
            }
            else if (n == 0) {
                perror("send: 合法失败");
                break;
            }
            else {
                if (errno == EAGAIN) {
                    pending_fds.insert(fd);
                    break;
                }
                else if (errno == EPIPE) {
                    item.conn_->pending_close_ = true;
                    sent = total;
                }
                else {
                    perror("send");
                    item.conn_->pending_close_ = true;
                    sent = total;
                }
            }
        }

        if (sent >= total) {
            // 如果发送完并存在关闭信号，断开连接
            if (item.conn_->pending_close_) {
                del_connection(item.conn_);
            }
        }
        // 为没发完的连接注册写事件
        else {
            // 将剩余信息记录到 pending_writes_
            std::string remainder = wire.substr(sent);
            auto [it, inserted] = this->pending_writes_.try_emplace(fd, std::move(remainder));
            if (!inserted) {
                it->second.append(wire.substr(sent));
            }
            // 注册写事件
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET | EPOLLOUT);
            // 立即尝试冲刷——解决 EPOLLET 竞态：socket 在 EAGAIN 与 mod_event
            // 之间变可写时不会再次触发 EPOLLOUT
            this->handle_write(item.conn_);
        }
    }
}
// 高优先的信息先发，低优先后发，虽然串行感觉有点自欺欺人
void Reactor::flush_responses() {
    if (this->flushing_) return;
    this->flushing_ = true;
    // 高优先级队列：聊天消息、房间管理、文件控制等 TEXT 帧
    drain_one(this->queue_high_,
              [this](const auto& c) { this->del_connection(c); });
    // 低优先级队列：BINARY 文件数据块及其 DWDATA 元数据
    drain_one(this->queue_low_,
              [this](const auto& c) { this->del_connection(c); });
    this->flushing_ = false;
}

// ======================================== 写事件回调 ========================================

void Reactor::handle_write(const std::shared_ptr<Connection>& conn) {
    int fd = conn->fd_;

    auto it = this->pending_writes_.find(fd);
    if (it == this->pending_writes_.end()) return;

    std::string& data = it->second;
    ssize_t total = data.size();
    ssize_t sent = 0;

    while (sent < total) {
        if (ssize_t n = send(fd, data.data() + sent, total - sent, 0);
                    n > 0) {
            sent += n;
        } 
        else if (n == 0) {
            perror("send: 合法失败");
            break;
        } 
        else {
            if (errno == EAGAIN) {
                break;
            }
            else if (errno == EPIPE) {
                this->pending_writes_.erase(fd);
                this->del_connection(conn);
                return;
            } 
            else {
                perror("send");
                this->pending_writes_.erase(fd);
                this->del_connection(conn);
                return;
            }
        }
    }
    // 发送完成
    if (sent >= total) {
        this->pending_writes_.erase(fd);
        // 如果句柄还有效就删除写事件
        if (this->loop_.has_event(fd)) {
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET);
        }
        // 连接可写了，处理之前因 EAGAIN 被重入队列的消息
        this->flush_responses();
        if (conn->pending_close_) {
            this->del_connection(conn);
        }
    } 
    else {
        data.erase(0, sent);
    }
}

// ======================================== 读取数据 ========================================

bool Reactor::read_data(const std::shared_ptr<Connection>& conn) {
    int clientfd = conn->fd_;
    char temp_buffer[BUFFER_SIZE];

    while (true) {
        if (ssize_t bytes_recv = recv(clientfd, temp_buffer, sizeof(temp_buffer), 0);
                    bytes_recv > 0) {
            conn->read_buf_.append(temp_buffer, bytes_recv);
        } 
        else if (bytes_recv == 0) {
            this->disconnect_connection(conn);
            return false;
        } 
        else {
            if (errno == EAGAIN) {
                break;
            }
            perror("recv");
            this->disconnect_connection(conn);
            return false;
        }
    }
    return true;
}

// ======================================== HTTP 请求处理 ========================================

void Reactor::handle_http(const std::shared_ptr<Connection>& conn) {
    int clientfd = conn->fd_;

    while (true) {
        HttpResult result = HttpParser::handle(conn->read_buf_);

        switch (result.type_) {
            // 未完成直接结束
            case HttpResultType::INCOMPLETE:{
                return;
            }
            // 错误的 HTTP 请求可能是网络问题或者网络攻击，直接断开好了
            case HttpResultType::BAD_REQUEST: {
                auto resp = ErrorResponse::bad_request(result.error_msg_);
                std::string wire = resp.serialize();
                send(clientfd, wire.data(), wire.size(), 0);
                this->del_connection(conn);
                return;
            }
            // 处理 WebSocket 协议升级请求
            case HttpResultType::WS_UPGRADE: {
                // 构建并发送响应
                auto resp = WebSocketUpgradeResponse::build(result.request_);
                std::string wire = resp.serialize();
                send(clientfd, wire.data(), wire.size(), 0);
                // 如果响应结果的状态码为 101，说明协议升级成功
                if (resp.status_ == 101) {
                    conn->read_buf_.erase(0, result.finished_);
                    // 将连接模式切换成 WebSocket 协议
                    conn->ws_mode_ = true;
                } 
                // 升级失败直接断开连接
                else {
                    this->del_connection(conn);
                }
                return;
            }
            // 处理普通的 HTTP 协议
            case HttpResultType::OK: {
                conn->read_buf_.erase(0, result.finished_);
                // 提交给线程池解析，生成响应
                this->works_.submit([this, conn, req = std::move(result.request_)]() {
                    HttpResponse resp = this->router_.handle(req);
                    std::string wire = resp.serialize();
                    // 获取响应再线程池将后续的响应工作交给事件循环
                    this->loop_.run_in_loop([this, conn, wire = std::move(wire)]() {
                        this->push_response(conn, std::move(wire), true);
                        this->flush_responses();
                    });
                });
                break;
            }
        }
    }
}

// ======================================== WebSocket 帧处理 ========================================
// TEXT 和 BINARY 在同一个线程池任务中顺序处理
// TEXT 统一走 WebSocketRouter 路由，含 CHUNK 命令
// BINARY 数据通过 TransferManager 转发给下载方

void Reactor::handle_ws(const std::shared_ptr<Connection>& conn) {
    auto ws_result = WebSocketParser::handle(conn->read_buf_, &conn->ws_frag_);
    conn->read_buf_.erase(0, ws_result.consumed_);

    bool has_binary = !ws_result.binary_messages_.empty();
    bool has_text = !ws_result.messages_.empty();
    bool has_ctrl = ws_result.ping_ || ws_result.close_;

    if (!has_binary && !has_text && !has_ctrl) return;

    this->works_.submit([this, conn,
                         binary = std::move(ws_result.binary_messages_),
                         msgs = std::move(ws_result.messages_),
                         ping = ws_result.ping_,
                         ping_payload = std::move(ws_result.ping_payload_),
                         close = ws_result.close_,
                         close_payload = std::move(ws_result.close_payload_)]() {
        std::vector<WebSocketTargetedMessage> text_results;
        std::vector<std::function<void()>> io_actions;

        // ---- 1. PONG ----
        if (ping) {
            std::string pong = WebSocketFrame::build(WebSocketOpcode::PONG, ping_payload);
            io_actions.push_back([this, conn, pong = std::move(pong)]() {
                this->push_response(conn, std::move(pong), true);
            });
        }

        // ---- 2. 文本消息和文件流 - 全部通过 WebSocketRouter ----
        // 每条消息单独转发，route 内部 out = handler() 会替换而非追加
        for (auto& text : msgs) {
            std::vector<WebSocketTargetedMessage> per_msg;
            this->ws_router_.route(WebSocketAppParser::parse(text), conn,
                                   this->room_mgr_,
                                   this->transfer_mgr_,
                                   per_msg);
            text_results.insert(text_results.end(),
                                std::make_move_iterator(per_msg.begin()),
                                std::make_move_iterator(per_msg.end()));
        }

        // ---- 3. BINARY 数据 - 检查是否为传输分块 ----
        for (auto& chunk : binary) {
            auto result = this->transfer_mgr_.handle_chunk_data(chunk);
            if (result.valid_) {
                // 在线程池中立即将 fd 解析为 shared_ptr，避免 IO 线程执行时 fd 被重用
                std::shared_ptr<Connection> dl_conn;
                if (auto c = this->conn_registry_.get(result.downloader_fd_)) {
                    dl_conn = std::move(c);
                }
                if (dl_conn) {
                    std::string dwdata = WebSocketFrame::build(WebSocketOpcode::TEXT,
                        WebSocketAppParser::build("DWDATA",
                            std::to_string(result.session_id_),
                            result.file_id_,
                            std::to_string(result.offset_),
                            std::to_string(result.size_)));
                    auto forward_data = TransferManager::wrap_chunk_data(
                        result.session_id_, result.offset_, result.data_);
                    std::string bin = WebSocketFrame::build(WebSocketOpcode::BINARY,
                                                            forward_data);
                    io_actions.push_back([this, dl_conn,
                                          dwdata = std::move(dwdata),
                                          bin = std::move(bin)]() {
                        if (dl_conn->alive_) {
                            this->push_response(dl_conn, std::move(dwdata), false);
                            this->push_response(dl_conn, std::move(bin), false);
                        }
                    });
                }
                // 滑动窗口有空位时发送下一个 DWREQ 给上传方
                if (result.has_next_req_) {
                    if (auto uploader_conn = this->conn_registry_.get(result.next_req_uploader_fd_)) {
                        std::string dwreq = WebSocketFrame::build(WebSocketOpcode::TEXT,
                            WebSocketAppParser::build("DWREQ",
                                std::to_string(result.next_req_session_id_),
                                result.file_id_,
                                std::to_string(result.next_req_offset_),
                                std::to_string(result.next_req_size_)));
                        io_actions.push_back([this, uploader_conn, dwreq = std::move(dwreq)]() {
                            if (uploader_conn->alive_) {
                                this->push_response(uploader_conn, std::move(dwreq), true);
                            }
                        });
                    }
                }
            }
        }

        // ---- 4. 投递 TEXT 路由结果，全为高优先级 ----
        if (!text_results.empty()) {
            io_actions.push_back([this, results = std::move(text_results)]() {
                for (auto& r : results) {
                    this->push_response(r.target_, std::move(r.data_), true);
                }
            });
        }

        // ---- 5. CLOSE 帧 -- 必须在 io_actions 刷新前处理，否则 DWERR 被丢弃
        if (close) {
            auto cancel_info = this->transfer_mgr_.cancel_by_fd(conn->fd_);
            for (auto& sc : cancel_info.cancelled_) {
                if (auto oc = this->conn_registry_.get(sc.orphaned_downloader_fd_)) {
                    std::string dwerr = WebSocketFrame::build(WebSocketOpcode::TEXT,
                        WebSocketAppParser::build("DWERR", sc.file_id_, "上传方已离开，下载失败"));
                    io_actions.push_back([this, oc, dwerr = std::move(dwerr)]() {
                        if (oc->alive_) {
                            this->push_response(oc, std::move(dwerr), true);
                        }
                    });
                }
            }

            // 捕获本地副本避免与 IO 线程的 disconnect 竞争
            std::string room_id = conn->room_id_;
            std::string nick = conn->nickname_;

            if (!room_id.empty()) {
                auto remaining = this->room_mgr_.leave_room(room_id, conn);
                conn->room_id_.clear();

                if (!remaining.empty()) {
                    std::string sys = WebSocketFrame::build(WebSocketOpcode::TEXT,
                        WebSocketAppParser::build("SYS", nick + " 离开房间"));

                    std::string joined;
                    for (size_t i = 0; i < remaining.size(); ++i) {
                        if (i > 0) joined += ",";
                        joined += std::to_string(remaining[i]->fd_) + ":" + remaining[i]->nickname_;
                    }
                    std::string members_frame = WebSocketFrame::build(WebSocketOpcode::TEXT,
                        WebSocketAppParser::build("MEMBERS", joined));

                    std::vector<std::shared_ptr<Connection>> targets;
                    for (auto& c : remaining) {
                        if (auto cc = this->conn_registry_.get(c->fd_)) {
                            targets.push_back(std::move(cc));
                        }
                    }

                    // LEAVE 帧用于客户端精确匹配上传方离开
                    std::string leave_frame = WebSocketFrame::build(WebSocketOpcode::TEXT,
                        WebSocketAppParser::build("LEAVE",
                            std::to_string(conn->fd_), nick));

                    this->loop_.run_in_loop([this, targets = std::move(targets),
                                             leave_frame,
                                             sys, members_frame]() {
                        for (auto& c : targets) {
                            if (c->alive_) {
                                this->push_response(c, leave_frame, true);
                                this->push_response(c, sys, true);
                                this->push_response(c, members_frame, true);
                            }
                        }
                        this->flush_responses();
                    });
                }
            }

            conn->pending_close_ = true;
            std::string close_frame = WebSocketFrame::build(WebSocketOpcode::CLOSE, close_payload);
            this->loop_.run_in_loop([this, conn, close_frame = std::move(close_frame)]() {
                this->push_response(conn, std::move(close_frame), true);
                this->flush_responses();
            });
        }

        // ---- 刷新 IO 操作 ----
        if (!io_actions.empty()) {
            this->loop_.run_in_loop([this, actions = std::move(io_actions)]() {
                for (auto& action : actions) {
                    action();
                }
                this->flush_responses();
            });
        }
    });
}

// ======================================== 客户端数据总入口 ========================================

void Reactor::handle_clientfd(const std::shared_ptr<Connection>& conn) {
    // 啥也没读到，直接返回
    if (!this->read_data(conn) || conn->read_buf_.empty()) {
        return;
    }
    // 根据连接协议选择不同的处理方式
    else{
        if (conn->ws_mode_) {
            this->handle_ws(conn);
        } 
        else {
            this->handle_http(conn);
        }
    }
}

// ======================================== 断开连接 ========================================

void Reactor::disconnect_connection(const std::shared_ptr<Connection>& conn) {
    // 先断开连接，防止后续 IO action 误发
    this->del_connection(conn);

    std::string room_id = conn->room_id_;
    std::string nick = conn->nickname_;
    this->works_.submit([this, conn, room_id = std::move(room_id), nick = std::move(nick)]() {
        // 1. 始终清理传输，room_id 为空时之前会漏掉
        auto cancel_info = this->transfer_mgr_.cancel_by_fd(conn->fd_);

        // 2. 收集受影响的下载方通知
        std::vector<std::pair<std::shared_ptr<Connection>, std::string>> dwerr_msgs;
        for (auto& sc : cancel_info.cancelled_) {
            std::string dwerr = WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("DWERR", sc.file_id_, "上传方已离开，下载失败"));
            if (auto oc = this->conn_registry_.get(sc.orphaned_downloader_fd_)) {
                dwerr_msgs.push_back({std::move(oc), std::move(dwerr)});
            }
        }

        // 3. 如果没加入房间，只发 DWERR 后返回
        if (room_id.empty()) {
            if (!dwerr_msgs.empty()) {
                this->loop_.run_in_loop([this, dwerr = std::move(dwerr_msgs)]() {
                    for (auto& [c, msg] : dwerr) {
                        if (c->alive_) this->push_response(c, msg, true);
                    }
                    this->flush_responses();
                });
            }
            return;
        }

        auto remaining = this->room_mgr_.leave_room(room_id, conn);
        if (remaining.empty() && dwerr_msgs.empty()) return;

        std::string sys, members_frame;
        if (!remaining.empty()) {
            sys = WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("SYS", nick + " 离开房间"));

            std::string joined;
            for (size_t i = 0; i < remaining.size(); ++i) {
                if (i > 0) joined += ",";
                joined += remaining[i]->nickname_;
            }
            members_frame = WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("MEMBERS", joined));
        }

        std::vector<std::shared_ptr<Connection>> room_targets;
        for (auto& c : remaining) {
            if (auto cc = this->conn_registry_.get(c->fd_)) {
                room_targets.push_back(std::move(cc));
            }
        }

        // LEAVE 帧用于客户端精确匹配上传方离开，不依赖昵称
        std::string leave_frame = WebSocketFrame::build(WebSocketOpcode::TEXT,
            WebSocketAppParser::build("LEAVE",
                std::to_string(conn->fd_), nick));

        this->loop_.run_in_loop([this, dwerr_msgs = std::move(dwerr_msgs),
                                 room_targets = std::move(room_targets),
                                 leave_frame,
                                 sys, members_frame]() {
            for (auto& [c, msg] : dwerr_msgs) {
                if (c->alive_) this->push_response(c, msg, true);
            }
            for (auto& c : room_targets) {
                if (c->alive_) {
                    this->push_response(c, leave_frame, true);
                    this->push_response(c, sys, true);
                    this->push_response(c, members_frame, true);
                }
            }
            this->flush_responses();
        });
    });
}
