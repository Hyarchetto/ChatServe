// 单 Reactor 多线程 TCP 服务器
// 所有连接在同一个 EventLoop 里管理，需要耗时操作就丢线程池
// HTTP 协议 → HttpParser → Router → HttpResponse → 回复
// WebSocket → WebSocketParser → WebSocketRouter → 聊天室路由 → 回复
// BINARY 帧 → FileManager → 文件上传
#pragma once

#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>
#include <thread>
#include <cstdint>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

#include "Connection.h"
#include "../core/ThreadPool.h"
#include "../core/EventLoop.h"
#include "../http/Router.h"
#include "../http/HttpParser.h"
#include "../http/StaticFileServer.h"
#include "../http/ErrorResponse.h"
#include "../ws/WebSocketParser.h"
#include "../ws/WebSocketFrame.h"
#include "../ws/WebSocketUpgradeResponse.h"
#include "../ws/WebSocketAppParser.h"
#include "../ws/WebSocketRouter.h"
#include "../chatroom/Room.h"
#include "../filetransfer/FileManager.h"

class Reactor {
public:
    Reactor();
    ~Reactor();

    // 创建 epoll 和 eventfd
    bool init();

    // 创建 socket、bind、listen，然后注册到 EventLoop
    void start_listen(int port);

    // 事件循环主函数，阻塞直到服务器退出
    void loop();

    // 添加一个客户端连接到本 Reactor
    // 可以被跨线程调用，内部走 run_in_loop
    void add_connection(int fd);

    // 向某个连接发数据（跨线程安全）
    void post_response(int fd, std::string data);

    // 向多个连接发相同数据（跨线程安全）
    void post_responses(const std::vector<int>& fds, const std::string& data);

private:
    // ==================== 核心组件 ====================

    EventLoop loop_;
    ThreadPool works_;
    FileManager file_mgr_;
    RoomManager room_mgr_;

    // ==================== 连接管理 ====================

    std::unordered_map<int, std::shared_ptr<Connection>> connections_;

    // 待发送的响应队列
    struct PendingResponse {
        std::shared_ptr<Connection> conn_;
        std::string data_;
    };
    std::queue<PendingResponse> queue_resp_;

    // 一次 send 没发完的数据暂存这里，等 EPOLLOUT 接着发
    std::unordered_map<int, std::string> pending_writes_;

    // ==================== 方法 ====================

    // 发送响应
    void flush_responses();
    void handle_write(const std::shared_ptr<Connection>& conn);

    // IO 处理
    bool read_data(const std::shared_ptr<Connection>& conn);
    void handle_http(const std::shared_ptr<Connection>& conn);
    void handle_ws(const std::shared_ptr<Connection>& conn);
    void handle_clientfd(const std::shared_ptr<Connection>& conn);

    // 连接生命周期
    void disconnect_connection(const std::shared_ptr<Connection>& conn);
    void del_connection(const std::shared_ptr<Connection>& conn);

    // 文件上传完成后的房间广播
    void broadcast_file_notification(const std::string& file_id, int exclude_fd);

    // ==================== Accept ====================

    int listenfd_ = -1;
    void accept_connections();

    // ==================== 路由 ====================

    Router router_;
    WebSocketAppParser ws_app_parser_;
    WebSocketRouter ws_router_;

    static constexpr int BUFFER_SIZE = 4096;
};
