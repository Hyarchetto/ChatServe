// 单 Reactor 多线程 TCP 服务器
// 所有连接在同一个 EventLoop 里管理
// HTTP 请求解析后丢线程池做路由，IO 线程只负责收发
// WebSocket 帧解析后也在线程池里处理业务，结果通过 run_in_loop 投回 IO 线程发送
//
// 跨线程通信全靠 EventLoop::run_in_loop，没有共享锁
#include "server/Reactor.h"

#include <iostream>
#include <cstdio>

// ======================================== 构造/析构 ========================================

Reactor::Reactor()
    : router_(&file_mgr_) {
}

Reactor::~Reactor() {
    this->loop_.quit();
    this->works_.shutdown();
}

bool Reactor::init() {
    return this->loop_.init();
}

// ======================================== 监听 ========================================

void Reactor::start_listen(int port) {
    this->listenfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (this->listenfd_ < 0) {
        perror("socket error");
        return;
    }

    int opt = 1;
    setsockopt(this->listenfd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(this->listenfd_, (struct sockaddr*)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind error");
        close(this->listenfd_);
        this->listenfd_ = -1;
        return;
    }
    if (listen(this->listenfd_, 1024) < 0) {
        perror("listen error");
        close(this->listenfd_);
        this->listenfd_ = -1;
        return;
    }

    int flags = fcntl(this->listenfd_, F_GETFL, 0);
    fcntl(this->listenfd_, F_SETFL, flags | O_NONBLOCK);

    this->loop_.add_event(this->listenfd_, EPOLLIN | EPOLLET,
        [this]() { this->accept_connections(); });

    std::cout << "服务器开始监听 " << port << std::endl;
    std::cout << "------------------------------------------" << std::endl;
}

void Reactor::accept_connections() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (true) {
        int clientfd = accept(this->listenfd_,
                              (struct sockaddr*)&client_addr,
                              &client_len);
        if (clientfd < 0) {
            if (errno == EAGAIN) break;
            if (errno == EINTR) continue;
            perror("accept error");
            break;
        }
        this->add_connection(clientfd);
    }
}

void Reactor::loop() {
    this->loop_.loop();
}

// ======================================== 连接管理 ========================================

void Reactor::add_connection(int fd) {
    this->loop_.run_in_loop([this, fd]() {
        auto conn = std::make_shared<Connection>(fd);
        this->connections_[fd] = conn;

        this->loop_.add_event(fd, EPOLLIN | EPOLLET,
            [this, conn]() { this->handle_clientfd(conn); },
            [this, conn]() { this->handle_write(conn); },
            [this, conn]() { this->disconnect_connection(conn); });
    });
}

void Reactor::del_connection(const std::shared_ptr<Connection>& conn) {
    int fd = conn->fd_;
    this->pending_writes_.erase(fd);
    this->connections_.erase(fd);
    this->loop_.del_event(fd);
}

// ======================================== 跨线程响应投递 ========================================

void Reactor::post_response(int fd, std::string data) {
    this->loop_.run_in_loop([this, fd, data = std::move(data)]() {
        auto it = this->connections_.find(fd);
        if (it != this->connections_.end()) {
            this->queue_resp_.emplace(
                PendingResponse{it->second, std::move(data)});
            this->flush_responses();
        }
    });
}

void Reactor::post_responses(const std::vector<int>& fds,
                              const std::string& data) {
    this->loop_.run_in_loop([this, fds, data]() {
        for (int fd : fds) {
            auto it = this->connections_.find(fd);
            if (it != this->connections_.end()) {
                this->queue_resp_.emplace(
                    PendingResponse{it->second, data});
            }
        }
        this->flush_responses();
    });
}

// ======================================== 响应发送 ========================================

void Reactor::flush_responses() {
    std::queue<PendingResponse> local_queue{};
    std::swap(local_queue, this->queue_resp_);

    while (!local_queue.empty()) {
        auto item = std::move(local_queue.front());
        local_queue.pop();

        int fd = item.conn_->fd_;

        if (!this->loop_.has_event(fd)) {
            continue;
        }

        std::string& wire = item.data_;
        ssize_t total = wire.size();
        ssize_t sent = 0;

        while (sent < total) {
            ssize_t n = send(fd, wire.data() + sent, total - sent, 0);

            if (n > 0) {
                sent += n;
            } else if (n == 0) {
                perror("send: 合法失败");
                break;
            } else {
                if (errno == EAGAIN) {
                    break;
                } else if (errno == EPIPE || errno == ECONNRESET) {
                    item.conn_->pending_close_ = true;
                    sent = total;
                } else {
                    perror("send");
                    item.conn_->pending_close_ = true;
                    sent = total;
                }
            }
        }

        if (sent >= total) {
            if (item.conn_->pending_close_) {
                this->del_connection(item.conn_);
            }
        } else {
            std::string remainder = wire.substr(sent);
            auto [it, inserted] = this->pending_writes_.try_emplace(
                fd, std::move(remainder));
            if (!inserted) {
                it->second.append(wire.substr(sent));
            }
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET | EPOLLOUT);
        }
    }
}

// ======================================== 写事件回调 ========================================

void Reactor::handle_write(const std::shared_ptr<Connection>& conn) {
    int fd = conn->fd_;

    auto it = this->pending_writes_.find(fd);
    if (it == this->pending_writes_.end()) {
        return;
    }

    std::string& data = it->second;
    ssize_t total = data.size();
    ssize_t sent = 0;

    while (sent < total) {
        ssize_t n = send(fd, data.data() + sent, total - sent, 0);

        if (n > 0) {
            sent += n;
        } else if (n == 0) {
            perror("send: 合法失败");
            break;
        } else {
            if (errno == EAGAIN) {
                break;
            } else if (errno == EPIPE || errno == ECONNRESET) {
                this->pending_writes_.erase(fd);
                this->del_connection(conn);
                return;
            } else {
                perror("send");
                this->pending_writes_.erase(fd);
                this->del_connection(conn);
                return;
            }
        }
    }

    if (sent >= total) {
        this->pending_writes_.erase(fd);

        if (this->loop_.has_event(fd)) {
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET);
        }

        if (conn->pending_close_) {
            this->del_connection(conn);
        }
    } else {
        data.erase(0, sent);
    }
}

// ======================================== 文件完成通知广播 ========================================

void Reactor::broadcast_file_notification(const std::string& file_id,
                                          int exclude_fd) {
    FileMeta meta = this->file_mgr_.get_meta(file_id);
    if (meta.file_id_.empty()) return;

    std::string notify = WebSocketFrame::build(WebSocketOpcode::TEXT,
        WebSocketAppParser::build("FILE",
            meta.file_id_,
            meta.filename_,
            std::to_string(meta.filesize_),
            meta.sender_nickname_));

    auto room = this->room_mgr_.get_or_create(meta.room_id_);

    // 单 Reactor，所有连接都在本地，不需要按 Reactor 分组
    std::vector<int> target_fds;
    auto live = room->get_live_connections();
    for (auto& c : live) {
        if (c->fd_ != exclude_fd) {
            target_fds.push_back(c->fd_);
        }
    }

    if (!target_fds.empty()) {
        // 直接用 run_in_loop 在 IO 线程发
        // 因为本函数可能在 thread pool 里被调用
        this->loop_.run_in_loop([this, fds = std::move(target_fds), notify]() {
            for (int fd : fds) {
                auto it = this->connections_.find(fd);
                if (it != this->connections_.end()) {
                    this->queue_resp_.emplace(
                        PendingResponse{it->second, notify});
                }
            }
            this->flush_responses();
        });
    }
}

// ======================================== 读取数据 ========================================

bool Reactor::read_data(const std::shared_ptr<Connection>& conn) {
    int clientfd = conn->fd_;
    char temp_buffer[BUFFER_SIZE];

    while (true) {
        ssize_t bytes_recv = recv(clientfd, temp_buffer, sizeof(temp_buffer), 0);
        if (bytes_recv > 0) {
            conn->read_buf_.append(temp_buffer, bytes_recv);
        } else if (bytes_recv == 0) {
            this->disconnect_connection(conn);
            return false;
        } else {
            if (errno == EAGAIN) break;
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
            case HttpResultType::INCOMPLETE:
                return;

            case HttpResultType::BAD_REQUEST: {
                auto resp = ErrorResponse::bad_request(result.error_msg_);
                std::string wire = resp.serialize();
                send(clientfd, wire.data(), wire.size(), 0);
                this->del_connection(conn);
                return;
            }

            case HttpResultType::WS_UPGRADE: {
                auto resp = WebSocketUpgradeResponse::build(result.request_);
                std::string wire = resp.serialize();
                send(clientfd, wire.data(), wire.size(), 0);

                if (resp.status_ == 101) {
                    conn->read_buf_.erase(0, result.finished_);
                    conn->ws_mode_ = true;
                } else {
                    this->del_connection(conn);
                }
                return;
            }

            case HttpResultType::OK: {
                conn->read_buf_.erase(0, result.finished_);

                // 所有 HTTP 请求统一走 Router
                // 文件下载也由 Router 内部处理
                this->works_.submit(
                    [this, conn, req = std::move(result.request_)]() {
                    HttpResponse resp = this->router_.handle(req);
                    std::string wire = resp.serialize();
                    this->loop_.run_in_loop(
                        [this, conn, wire = std::move(wire)]() {
                        this->queue_resp_.emplace(
                            PendingResponse{conn, std::move(wire)});
                        this->flush_responses();
                    });
                });
                break;
            }
        }
    }
}

// ======================================== WebSocket 帧处理 ========================================

void Reactor::handle_ws(const std::shared_ptr<Connection>& conn) {
    auto ws_result = WebSocketParser::handle(conn->read_buf_, &conn->ws_frag_);
    conn->read_buf_.erase(0, ws_result.consumed_);

    // BINARY 帧 = 文件上传数据
    for (auto& chunk : ws_result.binary_messages_) {
        this->works_.submit([this, conn, chunk = std::move(chunk)]() {
            auto result = this->file_mgr_.write_chunk(conn->fd_, chunk);
            if (result.received_ > 0) {
                std::string ack = WebSocketFrame::build(
                    WebSocketOpcode::TEXT,
                    WebSocketAppParser::build("UPCK",
                        std::to_string(result.received_)));
                this->loop_.run_in_loop(
                    [this, conn, ack = std::move(ack),
                     completed = result.completed_,
                     file_id = result.file_id_]() {
                    this->queue_resp_.emplace(
                        PendingResponse{conn, std::move(ack)});

                    if (completed) {
                        this->broadcast_file_notification(
                            file_id, conn->fd_);
                    }

                    this->flush_responses();
                });
            }
        });
    }

    // TEXT 消息 + 控制帧
    if (!ws_result.messages_.empty() ||
        ws_result.ping_ || ws_result.close_) {
        this->works_.submit(
            [this, conn,
             msgs = std::move(ws_result.messages_),
             ping = ws_result.ping_,
             ping_payload = std::move(ws_result.ping_payload_),
             close = ws_result.close_,
             close_payload = std::move(ws_result.close_payload_)]() {

            // PONG
            if (ping) {
                std::string pong = WebSocketFrame::build(
                    WebSocketOpcode::PONG, ping_payload);
                this->loop_.run_in_loop(
                    [this, conn, pong = std::move(pong)]() {
                    this->queue_resp_.emplace(
                        PendingResponse{conn, std::move(pong)});
                    this->flush_responses();
                });
            }

            // 路由应用层消息
            for (auto& text : msgs) {
                WebSocketAppMessage app_msg =
                    this->ws_app_parser_.parse(text);
                std::vector<WebSocketTargetedMessage> results;
                if (this->ws_router_.route(app_msg, conn,
                                           this->room_mgr_,
                                           this->file_mgr_,
                                           results)) {
                    this->loop_.run_in_loop(
                        [this, results = std::move(results)]() {
                        for (auto& r : results) {
                            // 单 Reactor，所有连接都在本地
                            // 直接入队就行，不用查 ConnectionRouter
                            this->queue_resp_.emplace(
                                PendingResponse{
                                    std::move(r.target_),
                                    std::move(r.data_)});
                        }
                        this->flush_responses();
                    });
                }
            }

            // CLOSE 帧
            if (close) {
                this->file_mgr_.cancel_upload(conn->fd_);

                if (!conn->room_id_.empty()) {
                    auto room = this->room_mgr_.get_or_create(
                        conn->room_id_);

                    room->del_num(conn);

                    auto remaining = room->get_live_connections();
                    if (remaining.empty()) {
                        this->room_mgr_.remove_if_empty(conn->room_id_);
                        conn->room_id_.clear();
                    } else {
                        std::string sys = WebSocketFrame::build(
                            WebSocketOpcode::TEXT,
                            WebSocketAppParser::build(
                                "SYS", conn->nickname_ + " 离开房间"));

                        std::string joined;
                        for (size_t i = 0; i < remaining.size(); ++i) {
                            if (i > 0) joined += ",";
                            joined += remaining[i]->nickname_;
                        }
                        std::string members_frame = WebSocketFrame::build(
                            WebSocketOpcode::TEXT,
                            WebSocketAppParser::build("MEMBERS", joined));

                        // 单 Reactor，直接收 fd 发
                        std::vector<int> fds;
                        for (auto& c : remaining) {
                            fds.push_back(c->fd_);
                        }

                        this->loop_.run_in_loop(
                            [this, fds, sys, members_frame]() {
                            for (int target_fd : fds) {
                                auto it = this->connections_.find(target_fd);
                                if (it != this->connections_.end()) {
                                    this->queue_resp_.emplace(
                                        PendingResponse{it->second, sys});
                                    this->queue_resp_.emplace(
                                        PendingResponse{
                                            it->second, members_frame});
                                }
                            }
                            this->flush_responses();
                        });

                        this->room_mgr_.remove_if_empty(conn->room_id_);
                        conn->room_id_.clear();
                    }
                }

                conn->pending_close_ = true;
                std::string close_frame = WebSocketFrame::build(
                    WebSocketOpcode::CLOSE, close_payload);
                this->loop_.run_in_loop(
                    [this, conn, close_frame = std::move(close_frame)]() {
                    this->queue_resp_.emplace(
                        PendingResponse{conn, std::move(close_frame)});
                    this->flush_responses();
                });
            }
        });
    }
}

// ======================================== 客户端数据总入口 ========================================

void Reactor::handle_clientfd(const std::shared_ptr<Connection>& conn) {
    if (!this->read_data(conn) || conn->read_buf_.empty()) {
        return;
    }
    if (!conn->ws_mode_) {
        this->handle_http(conn);
    } else {
        this->handle_ws(conn);
    }
}

// ======================================== 断开连接 ========================================

void Reactor::disconnect_connection(
    const std::shared_ptr<Connection>& conn) {
    this->file_mgr_.cancel_upload(conn->fd_);

    if (!conn->room_id_.empty()) {
        std::string room_id = conn->room_id_;
        std::string nick = conn->nickname_;
        this->works_.submit(
            [this, conn, room_id = std::move(room_id),
             nick = std::move(nick)]() {
            auto room = this->room_mgr_.get_or_create(room_id);

            room->del_num(conn);

            auto remaining = room->get_live_connections();
            if (remaining.empty()) {
                this->room_mgr_.remove_if_empty(room_id);
                conn->room_id_.clear();
                return;
            }

            std::string sys = WebSocketFrame::build(
                WebSocketOpcode::TEXT,
                WebSocketAppParser::build(
                    "SYS", nick + " 离开房间"));

            std::string joined;
            for (size_t i = 0; i < remaining.size(); ++i) {
                if (i > 0) joined += ",";
                joined += remaining[i]->nickname_;
            }
            std::string members_frame = WebSocketFrame::build(
                WebSocketOpcode::TEXT,
                WebSocketAppParser::build("MEMBERS", joined));

            // 单 Reactor，所有连接都在本地，直接按 fd 发
            std::vector<int> fds;
            for (auto& c : remaining) {
                fds.push_back(c->fd_);
            }

            this->loop_.run_in_loop(
                [this, fds, sys, members_frame]() {
                for (int target_fd : fds) {
                    auto it = this->connections_.find(target_fd);
                    if (it != this->connections_.end()) {
                        this->queue_resp_.emplace(
                            PendingResponse{it->second, sys});
                        this->queue_resp_.emplace(
                            PendingResponse{
                                it->second, members_frame});
                    }
                }
                this->flush_responses();
            });

            this->room_mgr_.remove_if_empty(room_id);
            conn->room_id_.clear();
        });
    }

    this->del_connection(conn);
}
