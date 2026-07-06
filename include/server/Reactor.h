// 单 Reactor 多线程 TCP 服务器 — 事件循环核心
#pragma once

#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <cstdint>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

#include "Connection.h"
#include "../core/ThreadPool.h"
#include "../http/Router.h"
#include "../http/HttpParser.h"
#include "../http/StaticFileServer.h"
#include "../ws/WebSocketParser.h"
#include "../ws/WebSocketFrame.h"
#include "../ws/WebSocketUpgradeResponse.h"
#include "../ws/WebSocketAppParser.h"
#include "../ws/WebSocketRouter.h"
#include "../chatroom/Room.h"

class Reactor {
public:
    Reactor();
    ~Reactor();

    // 初始化 epoll 和 eventfd
    bool init();

    // 启动监听（创建 socket、bind、listen）
    void start_listen(int port);

    // 事件循环
    void loop();

    // 通用事件注册（直接传回调，不暴露 Event 裸指针）
    void add_event(int fd, uint32_t events,
                   std::function<void()> read_cb = nullptr,
                   std::function<void()> write_cb = nullptr,
                   std::function<void()> err_cb = nullptr);
    // 便捷包装：自动将带 Connection 的回调包装为 void()
    void add_event(const std::shared_ptr<Connection>& cn, uint32_t events,
                   std::function<void(std::shared_ptr<Connection>)> read_cb = nullptr,
                   std::function<void(std::shared_ptr<Connection>)> write_cb = nullptr,
                   std::function<void(std::shared_ptr<Connection>)> err_cb = nullptr);

    // 删除事件（fd 版本 — 通用）
    void del_event(int fd);
    // 删除事件（Connection 版本 — 委托给 fd 版本）
    void del_event(const std::shared_ptr<Connection>& cn);

private:
    // ==================== Epoll 核心 ====================
    int epollfd_ = -1;
    int eventfd_ = -1;                // 用于唤醒事件循环
    // 回调结构体
    struct EventCallbacks {
        std::function<void()> readCb_;      // 读事件回调
        std::function<void()> writeCb_;     // 写事件回调
        std::function<void()> errCb_;       // 错误事件回调
    };
    std::unordered_map<int, EventCallbacks> event_map_;

    // ==================== 待发送的响应 ====================
    struct PendingResponse {
        std::shared_ptr<Connection> conn_;
        std::string data_;
    };

    std::unordered_map<int, std::string> pending_writes_;       // 未发送完的残留数据
    std::queue<PendingResponse> queue_resp_;                    // 线程池提交的响应队列
    std::mutex mtx_queue_resp_;                                 // 队列锁

    // ==================== 响应函数 ====================
    void flush_responses();

    // ==================== 响应队列回调 ====================
    void handle_eventfd();

    // ==================== 触发响应 ====================
    void trigger_eventfd();

    // ==================== 写事件回调 ====================
    void handle_write(const std::shared_ptr<Connection>& conn);

    // ==================== 接受连接 ====================
    void my_connect(const std::shared_ptr<Connection>& listen_conn);

    // ==================== 客户端数据处理 ====================
    // 数据读取（传输层）
    bool read_data(const std::shared_ptr<Connection>& conn);
    // HTTP 请求处理
    void handle_http(const std::shared_ptr<Connection>& conn);
    // WebSocket 帧处理
    void handle_ws(const std::shared_ptr<Connection>& conn);
    // 总入口：读取 + 协议分流
    void handle_clientfd(const std::shared_ptr<Connection>& client_conn);

    // ==================== 断开连接 ====================
    void disconnect_Connection(const std::shared_ptr<Connection>& conn);

    // ==================== 线程池 ====================
    ThdPool works_;

    // ==================== 路由 ====================
    Router router_;
    WebSocketAppParser ws_app_parser_;
    WebSocketRouter ws_router_;

    // ==================== 房间管理器 ====================
    RoomManager room_manager_;

    // ==================== 常量 ====================
    static constexpr int MAX_EVENTS = 1024;
    static constexpr int BUFFER_SIZE = 4096;
};