// 单Reactor多线程TCP服务器
// HTTP 协议 → HttpParser → Router（StaticFileServer / Handler）→ HttpResponse → Reactor 回复
// WebSocket → WebSocketParser → WebSocketRouter → WebSocketFrame → Reactor 回复

#include "server/Reactor.h"

#include <iostream>
#include <cstdio>

// ======================================== Reactor 构造/析构 ========================================

Reactor::Reactor() {
}

Reactor::~Reactor() {
    // 先等待线程池完成工作
    this->works_.shutdown();
    // 再关闭所有句柄
    close(this->epollfd_);
    close(this->eventfd_);
    // event_map_ 中的 std::function 自动析构清理
}

bool Reactor::init() {
    this->epollfd_ = epoll_create(1);
    if(this->epollfd_ < 0){
        perror("epoll_create");
        return false;
    }
    this->eventfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if(this->eventfd_ < 0){
        perror("eventfd");
        return false;
    }
    // 使用统一回调存储处理 eventfd，不再需要 loop 中的特判分支
    this->add_event(eventfd_, EPOLLIN,
        [this]() { this->handle_eventfd(); });
    return true;
}

// ======================================== 事件管理 ========================================

// 通用添加事件：需要直接传入fd句柄，不过直接存储回调，不涉及 Event 
void Reactor::add_event(int fd, uint32_t events,
                        std::function<void()> read_cb,
                        std::function<void()> write_cb,
                        std::function<void()> err_cb) {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events;

    this->event_map_.insert({fd, {std::move(read_cb), std::move(write_cb), std::move(err_cb)}});
    epoll_ctl(this->epollfd_, EPOLL_CTL_ADD, fd, &ev);
}

// 网络io的高级接口：将带 Connection 参数的回调包装为 void() 形式，不需要直接处理fd句柄，更安全
void Reactor::add_event(const std::shared_ptr<Connection>& cn, uint32_t events,
                        std::function<void(std::shared_ptr<Connection>)> read_cb,
                        std::function<void(std::shared_ptr<Connection>)> write_cb,
                        std::function<void(std::shared_ptr<Connection>)> err_cb) {
    // 用 lambda 包装回调函数，保证函数类型为 void()
    auto wrap = [&cn](auto&& cb) -> std::function<void()> {
        if (!cb){
            return nullptr;
        }
        return [conn = cn, cb = std::move(cb)]() { 
            cb(conn); 
        };
    };
    this->add_event(cn->fd_, events, wrap(read_cb), wrap(write_cb), wrap(err_cb));
}
// 删除事件（fd 版本）
void Reactor::del_event(int fd) {
    this->pending_writes_.erase(fd);
    epoll_ctl(this->epollfd_, EPOLL_CTL_DEL, fd, NULL);
    this->event_map_.erase(fd);
}

// 网络io的高级接口：删除事件，Connection 版本，虽然委托给 fd 版本，但运用时不需要直接接触网络io句柄
void Reactor::del_event(const std::shared_ptr<Connection>& cn) {
    this->del_event(cn->fd_);
}

// ======================================== 事件循环 ========================================

void Reactor::loop() {
    std::vector<epoll_event> evs(MAX_EVENTS);

    while (true) {
        // 响应由eventfd_驱动，epoll_wait直接阻塞
        int n = epoll_wait(this->epollfd_, evs.data(), MAX_EVENTS, -1);

        if (n < 0) {
            if (errno == EINTR){ 
                continue;
            }
            perror("epoll_wait");
            continue;
        }

        // 执行回调
        // 所有事件统一通过 EventCallbacks 的 std::function 直接分发
        for (int i = 0; i < n; ++i) {
            int fd = evs[i].data.fd;

            // 从 event_map_ 查找 EventCallbacks，防止在之前的回调中被删除
            auto it = this->event_map_.find(fd);
            if (it == this->event_map_.end()){
                continue;
            }
            auto& evcb = it->second;
            uint32_t flags = evs[i].events;
            // 错误事件
            if (flags & EPOLLERR) {
                if (evcb.errCb_){
                    evcb.errCb_();
                }
                continue;
            }
            // 读事件
            if (flags & EPOLLIN && evcb.readCb_) {
                evcb.readCb_();
            }
            // 写事件
            if (flags & EPOLLOUT) {
                auto wit = this->event_map_.find(fd);
                if (wit != this->event_map_.end() && wit->second.writeCb_) {
                    wit->second.writeCb_();
                }
            }
        }
    }
}

// ======================================== 监听 ========================================
void Reactor::start_listen(int port) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { 
        perror("socket error"); 
        return; 
    }
    // 服务器地址复用
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(lfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind error"); 
        return;
    }
    if (listen(lfd, 1024) < 0) {
        perror("listen error"); 
        return;
    }
    // 将成功开始监听的服务器套接字交给连接结构体管理
    auto listen_conn = std::make_shared<Connection>(lfd);
    // 注册对应的读事件
    this->add_event(listen_conn, EPOLLIN | EPOLLET,
                    std::bind(&Reactor::my_connect, this, listen_conn));

    std::cout << "服务器开始监听 " << port << std::endl;
    std::cout << "------------------------------------------" << std::endl;
}

// ================== 服务器端读事件回调，处理新的客户端连接 ==================

void Reactor::my_connect(const std::shared_ptr<Connection>& listen_conn) {
    int listenfd = listen_conn->fd_;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    // 所有的套接字设置为非阻塞边缘触发，循环读取直到EAGAIN
    while (true) {
        int clientfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_len);
        if (clientfd < 0) {
            // 没有新的连接
            if (errno == EAGAIN){
                break;
            }
            // 被信号打断
            else if (errno == EINTR){
                continue;
            }
            // 出现错误
            else{
                perror("accept error");
                break;
            }
        }
        // 将新的客户端套接字交给连接结构体保管
        auto client_conn = std::make_shared<Connection>(clientfd);
        // 在epoll中为客户端注册读事件和错误事件
        // 使用 lambda 避免 del_event 重载歧义
        this->add_event(client_conn, EPOLLIN | EPOLLET,
                        [this, conn = client_conn](auto) { this->handle_clientfd(conn); },
                        [this, conn = client_conn](auto) { this->handle_write(conn); },
                        [this, conn = client_conn](auto) { this->del_event(conn); });

        std::clog<<"新的客户端连接成功"<<std::endl;
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
        } 
        else if (bytes_recv == 0) {
            this->disconnect_Connection(conn);
            return false;
        } 
        else {
            if (errno == EAGAIN) break;
            perror("recv");
            this->disconnect_Connection(conn);
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
                auto resp = StaticFileServer::bad_request(result.error_msg_);
                std::string wire = resp.serialize();
                send(clientfd, wire.data(), wire.size(), 0);
                this->del_event(conn);
                return;
            }
            case HttpResultType::WS_UPGRADE: {
                auto resp = WebSocketUpgradeResponse::build(result.request_);
                std::string wire = resp.serialize();
                send(clientfd, wire.data(), wire.size(), 0);

                if (resp.status_ == 101) {
                    conn->read_buf_.erase(0, result.finished_);
                    conn->ws_mode_ = true;
                    std::cout << "WebSocket 握手成功, fd=" << clientfd << std::endl;
                }
                else {
                    this->del_event(conn);
                }
                return;
            }
            case HttpResultType::OK: {
                conn->read_buf_.erase(0, result.finished_);
                this->works_.submit([this, conn, req = std::move(result.request_)]() {
                    HttpResponse resp = this->router_.handle(req);
                    std::string wire = resp.serialize();
                    {
                        std::lock_guard<std::mutex> lock(this->mtx_queue_resp_);
                        this->queue_resp_.emplace(PendingResponse{conn, std::move(wire)});
                    }
                    this->trigger_eventfd();
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

    // 打印解码后的 WebSocket 消息
    // for (auto& msg : ws_result.messages_) {
    //     std::clog << "WebSocket 消息: \n" << msg << "\n" << std::endl;
    // }

    if (!ws_result.messages_.empty() || ws_result.ping_ || ws_result.close_) {
        this->works_.submit([this, conn,
                             msgs = std::move(ws_result.messages_),
                             ping = ws_result.ping_,
                             ping_payload = std::move(ws_result.ping_payload_),
                             close = ws_result.close_,
                             close_payload = std::move(ws_result.close_payload_)]() {
            // ---- PONG ----
            if (ping) {
                std::string pong = WebSocketFrame::build(WebSocketOpcode::PONG, ping_payload);
                {
                    std::lock_guard<std::mutex> lock(this->mtx_queue_resp_);
                    this->queue_resp_.emplace(PendingResponse{conn, std::move(pong)});
                }
                this->trigger_eventfd();
            }

            // ---- 应用层消息 ----
            for (auto& text : msgs) {
                WebSocketAppMessage app_msg = this->ws_app_parser_.parse(text);
                std::vector<WebSocketTargetedMessage> results;
                if (this->ws_router_.route(app_msg, conn, this->room_manager_, results)) {
                    {
                        std::lock_guard<std::mutex> lock(this->mtx_queue_resp_);
                        for (auto& r : results) {
                            this->queue_resp_.emplace(PendingResponse{r.target_, std::move(r.data_)});
                        }
                    }
                    this->trigger_eventfd();
                }
            }

            // ---- CLOSE ----
            if (close) {
                if (!conn->room_id_.empty()) {
                    std::string sys = WebSocketFrame::build(WebSocketOpcode::TEXT,
                        WebSocketAppParser::build("SYS", conn->nickname_ + " 离开房间"));
                    auto room = this->room_manager_.get_or_create(conn->room_id_);
                    {
                        std::lock_guard<std::mutex> lock(this->mtx_queue_resp_);
                        for (auto& c : room->get_live_Connections()) {
                            if (c->fd_ != conn->fd_) {
                                this->queue_resp_.emplace(PendingResponse{c, sys});
                            }
                        }
                    }
                    room->del_num(conn);
                    this->room_manager_.remove_if_empty(conn->room_id_);
                    conn->room_id_.clear();
                }

                conn->pending_close_ = true;
                std::string close_frame = WebSocketFrame::build(WebSocketOpcode::CLOSE, close_payload);
                {
                    std::lock_guard<std::mutex> lock(this->mtx_queue_resp_);
                    this->queue_resp_.emplace(PendingResponse{conn, std::move(close_frame)});
                }
                this->trigger_eventfd();
            }
        });
    }
}

// ======================================== 客户端数据总入口 ========================================

void Reactor::handle_clientfd(const std::shared_ptr<Connection>& client_conn) {
    std::clog << "触发客户端读事件" << std::endl;
    // 读取信息
    if (!read_data(client_conn) || client_conn->read_buf_.empty()){
        return;
    }
    // 按连接协议处理信息
    if (!client_conn->ws_mode_){
        // 如果是http协议，就直接输出日志
        // std::clog << "客户端信息：\n" << client_conn->read_buf_ << std::endl;
        handle_http(client_conn);
    }
    else{
        // ws协议还需要拆帧才能获取内容
        handle_ws(client_conn);
    }
}

// ======================================== 响应发送 ========================================

void Reactor::flush_responses() {
    std::queue<PendingResponse> local_queue{};
    {
        std::lock_guard<std::mutex> lock(this->mtx_queue_resp_);
        std::swap(local_queue, this->queue_resp_);
    }

    while (!local_queue.empty()) {
        auto item = std::move(local_queue.front());
        local_queue.pop();

        int fd = item.conn_->fd_;

        // 连接可能已经被关闭
        if (!event_map_.count(fd)) {
            continue;
        }

        std::string& wire = item.data_;
        ssize_t total = wire.size();
        ssize_t sent = 0;

        // 内层发送循环
        while (sent < total) {
            ssize_t n = send(fd, wire.data() + sent, total - sent, 0);

            if (n > 0) {
                sent += n;
            }
            else if (n == 0) {
                perror("send: 合法失败");
                break;
            }
            else {
                if (errno == EAGAIN) {
                    break;  // 写缓冲区满，进入 pending_writes_
                }
                else if (errno == EPIPE || errno == ECONNRESET) {
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

        // 发送完成
        if (sent >= total) {
            if (item.conn_->pending_close_) {
                this->del_event(item.conn_);
            }
        }
        else{
            // 未发送完，进入 pending_writes_
            std::string remainder = wire.substr(sent);
            auto [it, inserted] = pending_writes_.try_emplace(fd, std::move(remainder));
            if (!inserted) {
                it->second.append(wire.substr(sent));
            }
            // 注册写事件
            epoll_event ev{};
            ev.data.fd = fd;
            ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
            epoll_ctl(epollfd_, EPOLL_CTL_MOD, fd, &ev);
        }
    }
}

// ======================================== 写事件回调 ========================================

void Reactor::handle_write(const std::shared_ptr<Connection>& conn) {
    int fd = conn->fd_;

    auto it = pending_writes_.find(fd);
    if (it == pending_writes_.end()) {
        return;
    }

    std::string& data = it->second;
    ssize_t total = data.size();
    ssize_t sent = 0;

    while (sent < total) {
        ssize_t n = send(fd, data.data() + sent, total - sent, 0);

        if (n > 0) {
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
            else if (errno == EPIPE || errno == ECONNRESET) {
                pending_writes_.erase(fd);
                this->del_event(conn);
                return;
            }
            else {
                perror("send");
                pending_writes_.erase(fd);
                this->del_event(conn);
                return;
            }
        }
    }

    if (sent >= total) {
        pending_writes_.erase(fd);

        // 取消写事件
        if (auto ev_it = event_map_.find(fd); ev_it != event_map_.end()) {
            epoll_event ev{};
            ev.data.fd = fd;
            ev.events = EPOLLIN | EPOLLET;
            epoll_ctl(epollfd_, EPOLL_CTL_MOD, fd, &ev);
        }

        if (conn->pending_close_) {
            this->del_event(conn);
        }
    }
    else {
        data.erase(0, sent);
    }
}

// ======================================== 触发响应事件 ========================================

void Reactor::trigger_eventfd(){
    if(uint64_t x = 1; write(this->eventfd_, &x, sizeof(x)) < 0 && errno != EAGAIN){
        perror("write eventfd");
    }
}

// ======================================== 响应队列回调 ========================================

void Reactor::handle_eventfd(){
    // 取出数据
    if(uint64_t x{}; read(this->eventfd_, &x, sizeof(x)) < 0 && errno != EAGAIN){
        perror("read eventfd");
    }
    // 响应消息
    this->flush_responses();
}

// ======================================== ws断开连接 ========================================

void Reactor::disconnect_Connection(const std::shared_ptr<Connection>& conn) {
    // 将房间清理提交到线程池（避免阻塞 Reactor 线程）
    if (!conn->room_id_.empty()) {
        std::string room_id = conn->room_id_;
        std::string nick = conn->nickname_;
        this->works_.submit([this, conn, room_id = std::move(room_id), nick = std::move(nick)]() {
            auto room = this->room_manager_.get_or_create(room_id);
            std::string sys = WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("SYS", nick + " 离开房间"));
            {
                std::lock_guard<std::mutex> lock(this->mtx_queue_resp_);
                for (auto& c : room->get_live_Connections()) {
                    if (c->fd_ != conn->fd_) {
                        this->queue_resp_.emplace(PendingResponse{c, sys});
                    }
                }
            }
            this->trigger_eventfd();
            room->del_num(conn);
            this->room_manager_.remove_if_empty(room_id);
            conn->room_id_.clear();
        });
    }

    this->del_event(conn);
}
