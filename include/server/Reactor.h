// 子反应器
// 每个子 Reactor 包含一个 EventLoop 和一个线程池对象
// EventLoop 用于处理分配到本 Reactor 的所有客户端连接的 IO 事件
// 线程池用于处理耗时的业务逻辑，避免阻塞事件循环线程
//
// 这是整个服务器中最繁忙的模块，所有真正的数据收发都在这里完成
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
#include "../ws/WebSocketParser.h"
#include "../ws/WebSocketFrame.h"
#include "../ws/WebSocketUpgradeResponse.h"
#include "../ws/WebSocketAppParser.h"
#include "../ws/WebSocketRouter.h"
#include "../chatroom/Room.h"
#include "ConnectionRouter.h"

class FileManager;

class Reactor {
public:
    // 构造一个子 Reactor
    // conn_router 连接路由表，用于跨 Reactor 消息发送
    // room_mgr 房间管理器，所有 Reactor 共享同一个实例
    // file_mgr 文件管理器，所有 Reactor 共享同一个实例
    Reactor(ConnectionRouter* conn_router,
            RoomManager* room_mgr,
            FileManager* file_mgr);
    ~Reactor();

    // 启动本 Reactor 的事件循环线程
    // 事件循环在独立线程中运行，不会阻塞调用方
    void start();

    // 获取本 Reactor 的 EventLoop 指针
    EventLoop* loop() { return &this->loop_; }

    // 返回当前管理的客户端连接数量
    // 用于给 MainReactor 做负载感知分发
    size_t conn_count() const { return this->connections_.size(); }

    // ==================== 跨 Reactor 接口 ====================

    // 把一个新客户端连接注册到本 Reactor 中
    // 这个函数可以跨线程调用，内部通过 run_in_loop 投递
    // fd 是 accept 得到的客户端文件描述符
    void add_connection(int fd);

    // 向某个连接发送响应数据
    // 这个函数可以跨线程调用，内部通过 run_in_loop 投递
    // fd 是目标客户端，data 是要发送的完整响应字符串
    void post_response(int fd, std::string data);

    // 批量向多个连接发送同样的响应数据
    // 这个函数可以跨线程调用
    // 适用于房间广播场景
    void post_responses(const std::vector<int>& fds, const std::string& data);

private:
    // ==================== 核心组件 ====================

    EventLoop loop_;                    // 本 Reactor 的事件循环
    ThreadPool works_;                     // 本 Reactor 的线程池
    ConnectionRouter* conn_router_;     // 连接路由表
    RoomManager* room_mgr_;             // 聊天室管理器
    FileManager* file_mgr_;             // 文件传输管理器

    // ==================== 线程 ====================

    std::thread loop_thread_;           // 事件循环所在的线程

    // ==================== 连接管理 ====================

    // 本 Reactor 管理的所有客户端连接
    // key 是文件描述符，value 是连接对象
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;

    // ==================== 待发送的响应 ====================

    // 一条待发送的响应
    struct PendingResponse {
        std::shared_ptr<Connection> conn_;  // 目标连接
        std::string data_;                  // 要发送的数据
    };

    // 待发送的响应队列
    // queue_resp_ 中的数据通过 flush_responses 发送
    std::queue<PendingResponse> queue_resp_;

    // 发送未完成的数据缓冲区
    // 当一次 send 无法发完所有数据时，剩余部分暂存在这里
    // 然后注册 EPOLLOUT 事件等待 socket 可写时继续发送
    std::unordered_map<int, std::string> pending_writes_;

    // ==================== 响应发送 ====================

    // 把 queue_resp_ 队列中的数据通过 send 发送出去
    // 如果一次发不完，剩余部分存入 pending_writes_，
    // 并注册 EPOLLOUT 事件等待下次可写时继续发送
    void flush_responses();

    // ==================== 写事件回调 ====================

    // socket 可写时的回调函数
    // 继续发送 pending_writes_ 中未发送完的数据
    void handle_write(const std::shared_ptr<Connection>& conn);

    // ==================== 客户端数据处理 ====================

    // 从客户端 socket 中读取数据到 read_buf_ 缓冲区
    // 采用边缘触发模式，循环读取直到 EAGAIN
    // 返回 false 表示连接已断开或发生错误
    bool read_data(const std::shared_ptr<Connection>& conn);

    // 处理 HTTP 请求
    // 解析 HTTP 请求头，识别出是普通请求、WebSocket 升级、文件下载等
    // 普通请求交给线程池处理，文件下载和 WebSocket 升级可以直接发送
    void handle_http(const std::shared_ptr<Connection>& conn);

    // 处理 WebSocket 帧
    // 解析收到的帧数据，区分 TEXT BINARY PING CLOSE 等帧类型
    // 文本消息交给 WebSocketRouter 路由到对应的聊天室和用户
    // 二进制帧写入文件传输模块
    void handle_ws(const std::shared_ptr<Connection>& conn);

    // 客户端数据的统一入口函数
    // 先读数据，然后根据连接处于 HTTP 还是 WebSocket 模式分别处理
    void handle_clientfd(const std::shared_ptr<Connection>& conn);

    // ==================== 断开连接 ====================

    // 断开连接并做清理工作
    // 取消正在进行的文件上传，如果用户在聊天室中则广播离开消息
    void disconnect_connection(const std::shared_ptr<Connection>& conn);

    // 从 epoll 和连接表中彻底删除一个连接
    // 这个函数不发送任何消息，只是做清理
    void del_connection(const std::shared_ptr<Connection>& conn);

    // ==================== 文件传输辅助 ====================

    // 文件上传完成后向聊天室广播通知
    // 通知中包含文件名、文件大小、发送者等信息
    // 通过 ConnectionRouter 查找房间内各成员所属的 Reactor，
    // 分别发给对应的 Reactor
    void broadcast_file_notification(const std::string& file_id, int exclude_fd);

    // ==================== 路由 ====================

    // 每个 Reactor 有一个独立的路由器实例
    // 路由器中包含静态文件路径映射和 MIME 类型
    Router router_;
    WebSocketAppParser ws_app_parser_;  // WebSocket 应用层协议解析器
    WebSocketRouter ws_router_;         // WebSocket 消息路由器

    static constexpr int BUFFER_SIZE = 4096;  // 读取数据的临时缓冲区大小
};
