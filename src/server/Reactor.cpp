// 子反应器的实现
// 每个子 Reactor 运行在独立的线程中，
// 管理着一批客户端连接的 IO 事件和协议处理
//
// 数据处理流程：
//   handle_clientfd 是入口函数
//   先读数据，然后根据连接状态走 HTTP 或 WebSocket 路径
//   HTTP 请求解析后交给线程池做路由，在 IO 线程发送响应
//   WebSocket 帧解析后在 IO 线程做路由和跨 Reactor 转发
//
// 跨 Reactor 通信机制：
//   通过 ConnectionRouter 查询目标 fd 所属的 Reactor，
//   然后调用目标 Reactor 的 post_response 函数，
//   该函数通过 EventLoop 的 run_in_loop 机制投递到目标线程
#include "server/Reactor.h"
#include "filetransfer/FileManager.h"
#include "http/ErrorResponse.h"

#include <iostream>
#include <cstdio>

// ======================================== Reactor 构造/析构 ========================================

// 构造函数
// 初始化事件循环和路由表指针，事件循环的 init 在这里调用
Reactor::Reactor(ConnectionRouter* conn_router,
                 RoomManager* room_mgr,
                 FileManager* file_mgr)
    : conn_router_(conn_router),
      room_mgr_(room_mgr),
      file_mgr_(file_mgr),
      router_(file_mgr) {
    this->loop_.init();
}

// 析构函数
// 先退出事件循环，再等待线程结束，最后关闭线程池
// 确保资源被完整清理
Reactor::~Reactor() {
    this->loop_.quit();
    if (this->loop_thread_.joinable()) {
        this->loop_thread_.join();
    }
    this->works_.shutdown();
}

// 启动本 Reactor 的事件循环线程
// 在新线程中运行 EventLoop 的 loop 函数
// 这个函数不会阻塞调用方，调用后立即返回
void Reactor::start() {
    this->loop_thread_ = std::thread([this]() {
        this->loop_.loop();
    });
}

// ======================================== 连接管理 ========================================

// 添加一个新连接到本 Reactor
// 这个函数可以被跨线程调用，内部通过 run_in_loop 投递
//
// 投递后的执行流程：
//   1. 创建 Connection 对象封装这个 fd
//   2. 存入 connections_ 映射表
//   3. 在 ConnectionRouter 中登记 fd 到本 Reactor 的映射
//   4. 注册 fd 到 epoll，采用边缘触发模式监听可读事件
//      同时注册可写回调和错误回调
//
// 所有的 epoll 操作都在事件循环线程中执行，避免线程安全问题
void Reactor::add_connection(int fd) {
    this->loop_.run_in_loop([this, fd]() {
        auto conn = std::make_shared<Connection>(fd);
        this->connections_[fd] = conn;
        this->conn_router_->register_connection(fd, this);

        this->loop_.add_event(fd, EPOLLIN | EPOLLET,
            [this, conn]() { this->handle_clientfd(conn); },
            [this, conn]() { this->handle_write(conn); },
            [this, conn]() { this->disconnect_connection(conn); });
    });
}

// 从本 Reactor 中删除一个连接
// 清理操作包括：
//   从 pending_writes_ 中删除未发完的数据
//   从 connections_ 映射表中删除
//   从 ConnectionRouter 中删除路由记录
//   从 epoll 中删除事件监听
void Reactor::del_connection(const std::shared_ptr<Connection>& conn) {
    int fd = conn->fd_;
    this->pending_writes_.erase(fd);
    this->connections_.erase(fd);
    this->conn_router_->unregister_connection(fd);
    this->loop_.del_event(fd);
}

// ======================================== 跨 Reactor 响应投递 ========================================

// 向某个连接发送响应数据
// 这个函数可以被跨线程调用
// 内部通过 run_in_loop 把发送操作安全地投递到事件循环线程
// fd 不存在时直接忽略
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

// 向多个连接批量发送同样的响应数据
// 适用于房间广播场景，效率比逐个调用 post_response 高
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

// 发送响应队列中的所有数据
//
// 先把 queue_resp_ 整体交换到局部变量中，
// 这样可以尽快释放锁，减少锁持有时间
//
// 对每条待发送数据：
//   检查连接是否仍然有效，如果已从 epoll 中移除则跳过
//   调用 send 发送数据
//   如果 send 返回值大于 0 表示发送了部分或全部数据
//   如果 send 返回 EAGAIN 表示内核发送缓冲区已满
//     此时把剩余数据存入 pending_writes_，
//     并注册 EPOLLOUT 事件等待 socket 可写时继续发送
//   如果 send 返回 EPIPE 或 ECONNRESET 表示连接已断开
//     设置 pending_close_ 标记，在数据发完后关闭连接
//
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

        // 循环发送，每次发送尽量多的数据
        while (sent < total) {
            ssize_t n = send(fd, wire.data() + sent, total - sent, 0);

            if (n > 0) {
                sent += n;
            } else if (n == 0) {
                perror("send: 合法失败");
                break;
            } else {
                if (errno == EAGAIN) {
                    // 内核发送缓冲区满了
                    // 剩余数据稍后再发
                    break;
                } else if (errno == EPIPE || errno == ECONNRESET) {
                    // 对端关闭了连接
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
            // 全部发送完成
            if (item.conn_->pending_close_) {
                this->del_connection(item.conn_);
            }
        } else {
            // 没有发完，剩余数据暂存起来
            std::string remainder = wire.substr(sent);
            auto [it, inserted] = this->pending_writes_.try_emplace(
                fd, std::move(remainder));
            if (!inserted) {
                it->second.append(wire.substr(sent));
            }
            // 注册 EPOLLOUT 事件，等待 socket 可写时继续发送
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET | EPOLLOUT);
        }
    }
}

// ======================================== 文件完成通知广播 ========================================

// 文件上传完成后向房间广播通知消息
//
// 首先获取文件的元信息，然后构建 FILE 类型的 WebSocket 通知帧
// 获取房间内的所有在线连接，通过 ConnectionRouter 查询每个连接所属的 Reactor
// 按 Reactor 分组后：
//   属于本 Reactor 的连接直接放入发送队列
//   属于其他 Reactor 的连接通过 post_responses 投递
//
// exclude_fd 是上传者的 fd，通知不发给上传者本人
void Reactor::broadcast_file_notification(const std::string& file_id,
                                          int exclude_fd) {
    FileMeta meta = this->file_mgr_->get_meta(file_id);
    if (meta.file_id_.empty()) return;

    std::string notify = WebSocketFrame::build(WebSocketOpcode::TEXT,
        WebSocketAppParser::build("FILE",
            meta.file_id_,
            meta.filename_,
            std::to_string(meta.filesize_),
            meta.sender_nickname_));

    auto room = this->room_mgr_->get_or_create(meta.room_id_);

    // 按 Reactor 分组收集需要通知的 fd
    std::unordered_map<Reactor*, std::vector<int>> groups;
    {
        auto live = room->get_live_connections();
        for (auto& c : live) {
            if (c->fd_ == exclude_fd) continue;
            Reactor* owner = this->conn_router_->get_reactor(c->fd_);
            if (owner) {
                groups[owner].push_back(c->fd_);
            }
        }
    }

    // 按分组发送通知
    for (auto& [reactor, fds] : groups) {
        if (reactor == this) {
            for (int target_fd : fds) {
                auto it = this->connections_.find(target_fd);
                if (it != this->connections_.end()) {
                    this->queue_resp_.emplace(
                        PendingResponse{it->second, notify});
                }
            }
        } else {
            reactor->post_responses(fds, notify);
        }
    }
}

// ======================================== 写事件回调 ========================================

// socket 可写时的回调函数
//
// 当内核发送缓冲区有空间时 epoll 触发 EPOLLOUT 事件
// 此时继续发送 pending_writes_ 中未发完的数据
//
// 发送逻辑与 flush_responses 中的 send 循环一致
// 全部发完后移除 EPOLLOUT 事件监听，
// 如果还有数据未发完则继续保持 EPOLLOUT 监听
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
        // 全部发送完成，清理 pending_writes_
        this->pending_writes_.erase(fd);

        if (this->loop_.has_event(fd)) {
            // 移除 EPOLLOUT 事件，只保留可读事件
            this->loop_.mod_event(fd, EPOLLIN | EPOLLET);
        }

        if (conn->pending_close_) {
            this->del_connection(conn);
        }
    } else {
        // 还有数据没发完，更新已发送的部分
        data.erase(0, sent);
    }
}

// ======================================== 读取数据 ========================================

// 从客户端 socket 读取数据
//
// 由于采用边缘触发模式，使用 while 循环持续读取直到 EAGAIN
// 每次读取最多 BUFFER_SIZE 字节，追加到连接对象的读缓冲区中
//
// 返回值说明：
//   返回 true 表示读取正常，数据在 read_buf_ 中
//   返回 false 表示连接已关闭或发生错误，调用方应该停止处理
bool Reactor::read_data(const std::shared_ptr<Connection>& conn) {
    int clientfd = conn->fd_;
    char temp_buffer[BUFFER_SIZE];

    while (true) {
        ssize_t bytes_recv = recv(clientfd, temp_buffer, sizeof(temp_buffer), 0);
        if (bytes_recv > 0) {
            conn->read_buf_.append(temp_buffer, bytes_recv);
        } else if (bytes_recv == 0) {
            // 对端关闭连接
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

// 处理 HTTP 请求
//
// 采用循环解析，因为一个 TCP 数据包中可能包含多个 HTTP 请求
// 每个请求处理完后清空已消费的缓冲区数据
//
// 解析结果有四种情况：
//   INCOMPLETE 表示数据还不够解析一个完整的 HTTP 请求头
//     直接返回，等更多数据到达后再继续
//
//   BAD_REQUEST 表示请求格式错误
//     发送 400 响应后关闭连接
//
//   WS_UPGRADE 表示这是一个 WebSocket 升级请求
//     尝试升级连接，成功后把连接设为 ws_mode 模式
//
//   OK 表示这是一个正常的 HTTP 请求
//     根据路径判断是文件下载还是普通路由，
//     把处理任务交给线程池执行
//
// 工作流：
//   IO 线程负责解析和发送
//   线程池负责路由器处理和耗时业务逻辑
//   线程池处理完后再通过 run_in_loop 回到 IO 线程发送响应
//
void Reactor::handle_http(const std::shared_ptr<Connection>& conn) {
    int clientfd = conn->fd_;

    while (true) {
        HttpResult result = HttpParser::handle(conn->read_buf_);

        switch (result.type_) {
            case HttpResultType::INCOMPLETE:
                // 数据不足，等待下一次可读事件
                return;

            case HttpResultType::BAD_REQUEST: {
                // 请求格式错误，直接发 400 并断开
                auto resp = ErrorResponse::bad_request(result.error_msg_);
                std::string wire = resp.serialize();
                send(clientfd, wire.data(), wire.size(), 0);
                this->del_connection(conn);
                return;
            }

            case HttpResultType::WS_UPGRADE: {
                // WebSocket 升级请求
                auto resp = WebSocketUpgradeResponse::build(result.request_);
                std::string wire = resp.serialize();
                send(clientfd, wire.data(), wire.size(), 0);

                if (resp.status_ == 101) {
                    // 升级成功，切换到 WebSocket 模式
                    conn->read_buf_.erase(0, result.finished_);
                    conn->ws_mode_ = true;
                } else {
                    this->del_connection(conn);
                }
                return;
            }

            case HttpResultType::OK: {
                conn->read_buf_.erase(0, result.finished_);

                // 所有 HTTP 请求统一走 Router，包括文件下载
                // Router 内部会判断路径，文件下载就调 FileManager，普通请求就调 handler
                // 这样 Reactor 不用关心业务路径
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

// 处理 WebSocket 帧数据
//
// 从读缓冲区中解析出 WebSocket 帧，根据帧类型分别处理：
//
// BINARY 帧：
//   二进制数据通常是文件上传的分片
//   交给 FileManager 写入磁盘
//   每次写入后回复 UPCK 确认帧给发送者
//   如果文件已接收完成，广播 FILE 通知给聊天室
//
// TEXT 帧：
//   文本消息包含应用层协议
//   通过 WebSocketAppParser 解析出消息类型和内容
//   再通过 WebSocketRouter 路由到对应的聊天室或用户
//   路由结果可能包含多个目标用户，这些用户可能在不同 Reactor 上
//   属于本 Reactor 的用户直接放入发送队列
//   属于其他 Reactor 的用户通过 post_response 转发
//
// PING 帧：
//   回复 PONG 帧
//
// CLOSE 帧：
//   清理用户上传任务
//   如果用户在聊天室中，广播离开消息给房间所有人
//   回复 CLOSE 帧确认关闭
//
void Reactor::handle_ws(const std::shared_ptr<Connection>& conn) {
    auto ws_result = WebSocketParser::handle(conn->read_buf_, &conn->ws_frag_);
    conn->read_buf_.erase(0, ws_result.consumed_);

    // ---- 处理 BINARY 帧，文件上传数据 ----
    for (auto& chunk : ws_result.binary_messages_) {
        this->works_.submit([this, conn, chunk = std::move(chunk)]() {
            auto result = this->file_mgr_->write_chunk(conn->fd_, chunk);
            if (result.received_ > 0) {
                // 发送 UPCK 确认帧告诉发送者已接收了多少字节
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

                    // 文件接收完成，广播通知给房间
                    if (completed) {
                        this->broadcast_file_notification(
                            file_id, conn->fd_);
                    }

                    this->flush_responses();
                });
            }
        });
    }

    // ---- 处理 TEXT 消息和控制帧 ----
    if (!ws_result.messages_.empty() ||
        ws_result.ping_ || ws_result.close_) {
        this->works_.submit(
            [this, conn,
             msgs = std::move(ws_result.messages_),
             ping = ws_result.ping_,
             ping_payload = std::move(ws_result.ping_payload_),
             close = ws_result.close_,
             close_payload = std::move(ws_result.close_payload_)]() {

            // ---- 回复 PONG ----
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

            // ---- 路由应用层消息 ----
            for (auto& text : msgs) {
                WebSocketAppMessage app_msg =
                    this->ws_app_parser_.parse(text);
                std::vector<WebSocketTargetedMessage> results;
                if (this->ws_router_.route(app_msg, conn,
                                           *this->room_mgr_,
                                           *this->file_mgr_,
                                           results)) {
                    this->loop_.run_in_loop(
                        [this, results = std::move(results)]() {
                        // 路由结果可能包含多个目标
                        // 每个目标可能属于不同的 Reactor
                        for (auto& r : results) {
                            int target_fd = r.target_->fd_;
                            Reactor* owner =
                                this->conn_router_->get_reactor(
                                    target_fd);
                            if (owner == this) {
                                // 目标在本 Reactor，直接发送
                                this->queue_resp_.emplace(
                                    PendingResponse{
                                        std::move(r.target_),
                                        std::move(r.data_)});
                            } else if (owner) {
                                // 目标在其他 Reactor，跨线程投递
                                owner->post_response(
                                    target_fd, std::move(r.data_));
                            }
                        }
                        this->flush_responses();
                    });
                }
            }

            // ---- 处理 CLOSE 帧 ----
            // 关闭 WebSocket 连接，分两种情况：
            //   1. 用户在聊天室中 → 先移除、广播 MEMBERS + SYS、再回复 CLOSE
            //   2. 用户不在聊天室 → 直接回复 CLOSE
            // MEMBERS 全量推送，是唯一来源
            // SYS 只用来显示 "xxx 离开房间"
            if (close) {
                this->file_mgr_->cancel_upload(conn->fd_);

                if (!conn->room_id_.empty()) {
                    auto room = this->room_mgr_->get_or_create(
                        conn->room_id_);

                    // 先移人再拿列表，MEMBERS 才正确
                    room->del_num(conn);

                    auto remaining = room->get_live_connections();
                    if (remaining.empty()) {
                        this->room_mgr_->remove_if_empty(conn->room_id_);
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

                        // 按 Reactor 分组，同组同线程
                        std::unordered_map<Reactor*, std::vector<int>> groups;
                        for (auto& c : remaining) {
                            Reactor* owner =
                                this->conn_router_->get_reactor(c->fd_);
                            if (owner) {
                                groups[owner].push_back(c->fd_);
                            }
                        }

                        for (auto& [reactor, fds] : groups) {
                            if (reactor == this) {
                                this->loop_.run_in_loop(
                                    [this, fds, sys, members_frame]() {
                                    for (int target_fd : fds) {
                                        auto it =
                                            this->connections_.find(
                                                target_fd);
                                        if (it !=
                                            this->connections_.end()) {
                                            this->queue_resp_.emplace(
                                                PendingResponse{
                                                    it->second, sys});
                                            this->queue_resp_.emplace(
                                                PendingResponse{
                                                    it->second,
                                                    members_frame});
                                        }
                                    }
                                    this->flush_responses();
                                });
                            } else {
                                reactor->post_responses(fds, sys);
                                reactor->post_responses(fds, members_frame);
                            }
                        }

                        this->room_mgr_->remove_if_empty(conn->room_id_);
                        conn->room_id_.clear();
                    }
                }

                // 回复 CLOSE 帧并准备关闭
                conn->pending_close_ = true;
                std::string close_frame = WebSocketFrame::build(
                    WebSocketOpcode::CLOSE, close_payload);
                this->loop_.run_in_loop(
                    [this, conn,
                     close_frame = std::move(close_frame)]() {
                    this->queue_resp_.emplace(
                        PendingResponse{conn, std::move(close_frame)});
                    this->flush_responses();
                });
            }
        });
    }
}

// ======================================== 客户端数据总入口 ========================================

// 客户端数据的统一入口
//
// 当一个客户端 socket 可读时，epoll 触发这个回调
// 处理流程：
//   1. 调用 read_data 从 socket 读取数据
//   2. 如果读取失败或没有数据则返回
//   3. 根据连接模式进入 HTTP 或 WebSocket 处理路径
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

// 断开连接并执行清理操作
//
// 清理步骤：
//   1. 取消该连接正在进行的文件上传
//   2. 如果用户在聊天室中：
//      a. 从房间中移除用户
//      b. 把 MEMBERS 和 SYS 消息按 Reactor 分组并发给剩余成员
//      c. 房间空了就销毁房间
//   3. 从 epoll 和连接表中删除这个连接
//
// MEMBERS 每次离开都全量推送，是成员列表的唯一来源
// SYS 只用来在聊天框显示 "xxx 离开房间"
void Reactor::disconnect_connection(
    const std::shared_ptr<Connection>& conn) {
    // 取消未完成的文件上传
    this->file_mgr_->cancel_upload(conn->fd_);

    // 如果用户在聊天室中，广播离开消息
    if (!conn->room_id_.empty()) {
        std::string room_id = conn->room_id_;
        std::string nick = conn->nickname_;
        this->works_.submit(
            [this, conn, room_id = std::move(room_id),
             nick = std::move(nick)]() {
            auto room = this->room_mgr_->get_or_create(room_id);

            // 先移人再拿剩余列表，这样 MEMBERS 才是正确的
            room->del_num(conn);

            auto remaining = room->get_live_connections();
            if (remaining.empty()) {
                this->room_mgr_->remove_if_empty(room_id);
                conn->room_id_.clear();
                return;
            }

            // 组装 SYS 和 MEMBERS
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

            // 按 Reactor 分组，一个组里的 fd 同属一个线程，可以批量发
            std::unordered_map<Reactor*, std::vector<int>> groups;
            for (auto& c : remaining) {
                Reactor* owner =
                    this->conn_router_->get_reactor(c->fd_);
                if (owner) {
                    groups[owner].push_back(c->fd_);
                }
            }

            // 分 Reactor 发送 SYS + MEMBERS
            for (auto& [reactor, fds] : groups) {
                if (reactor == this) {
                    // 本 Reactor 直接入队
                    this->loop_.run_in_loop(
                        [this, fds, sys, members_frame]() {
                        for (int target_fd : fds) {
                            auto it = this->connections_.find(
                                target_fd);
                            if (it != this->connections_.end()) {
                                this->queue_resp_.emplace(
                                    PendingResponse{
                                        it->second, sys});
                                this->queue_resp_.emplace(
                                    PendingResponse{
                                        it->second,
                                        members_frame});
                            }
                        }
                        this->flush_responses();
                    });
                } else {
                    // 跨 Reactor 投递
                    reactor->post_responses(fds, sys);
                    reactor->post_responses(fds, members_frame);
                }
            }

            this->room_mgr_->remove_if_empty(room_id);
            conn->room_id_.clear();
        });
    }

    // 从 epoll 和连接表中删除
    this->del_connection(conn);
}
