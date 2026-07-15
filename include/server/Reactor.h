// 单 Reactor 多线程 TCP 服务器
// 所有连接在同一个 EventLoop 里管着，耗时的活直接丢线程池
// HTTP → HttpParser → Router → HttpResponse → 回复
// WebSocket → WebSocketParser → WebSocketRouter → 聊天室路由 → 回复
// BINARY 帧 → TransferManager → 滑动窗口转发
#pragma once

#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
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
#include "../ws/WebSocketRouter.h"
#include "../chatroom/Room.h"
#include "../transfer/TransferManager.h"

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

    // 优雅停止，退出事件循环 + 等待线程池收尾
    void stop();

    // 添加一个客户端连接到本 Reactor
    // 必须在 IO 线程调用，直接注册到 EventLoop
    void add_connection(int fd);

private:
    // ==================== 核心组件 ====================

    EventLoop loop_;
    ThreadPool works_;
    TransferManager transfer_mgr_;
    RoomManager room_mgr_;

    // ==================== 连接管理 ====================

    std::unordered_map<int, std::shared_ptr<Connection>> connections_;

    // 线程安全的 fd → Connection 映射，供线程池 worker 将 fd 解析为 shared_ptr
    // 解决 fd 重用竞态：IO 线程创建 IO action 时用的是解析后的 shared_ptr 而非原始 fd
    struct ConnRegistry {
        std::mutex mtx_;
        std::unordered_map<int, std::weak_ptr<Connection>> map_;

        void add(int fd, std::shared_ptr<Connection> conn) {
            std::lock_guard<std::mutex> lock(mtx_);
            map_[fd] = std::move(conn);
        }

        void remove(int fd) {
            std::lock_guard<std::mutex> lock(mtx_);
            map_.erase(fd);
        }

        // 返回 shared_ptr，若连接已断开或 fd 已被重用则返回 nullptr
        std::shared_ptr<Connection> get(int fd) {
            std::lock_guard<std::mutex> lock(mtx_);
            auto it = map_.find(fd);
            if (it == map_.end()) return nullptr;
            auto sp = it->second.lock();
            if (!sp) { map_.erase(it); return nullptr; }
            if (!sp->alive_) return nullptr;
            return sp;
        }
    };
    ConnRegistry conn_registry_;

    // 待发送的响应 — 高优先级队列先于低优先级发送
    // 高：聊天消息、房间管理、文件控制消息等 TEXT 帧
    // 低：BINARY 文件数据块及其 DWDATA 元数据
    struct PendingResponse {
        std::shared_ptr<Connection> conn_;
        std::string data_;
    };
    std::queue<PendingResponse> queue_high_;
    std::queue<PendingResponse> queue_low_;

    // 一次 send 没发完的数据暂存这里，等 EPOLLOUT 接着发
    std::unordered_map<int, std::string> pending_writes_;

    // flush_responses 重入守卫，防止 drain_one → handle_write → flush_responses 递归
    bool flushing_ = false;

    // ==================== 方法 ====================
    // 发送响应 — 高优先级队列先于低优先级发送
    void flush_responses();
    void push_response(const std::shared_ptr<Connection>& conn,
                        std::string data, bool is_high_priority);
    void drain_one(std::queue<PendingResponse>& q,
                   std::function<void(const std::shared_ptr<Connection>&)> dc);
    void handle_write(const std::shared_ptr<Connection>& conn);

    // IO 处理
    bool read_data(const std::shared_ptr<Connection>& conn);
    void handle_http(const std::shared_ptr<Connection>& conn);
    void handle_ws(const std::shared_ptr<Connection>& conn);
    void handle_clientfd(const std::shared_ptr<Connection>& conn);

    // 连接生命周期
    void disconnect_connection(const std::shared_ptr<Connection>& conn);
    void del_connection(const std::shared_ptr<Connection>& conn);

    // ==================== Accept ====================
    void accept_connections(int listenfd);

    // ==================== 路由 ====================
    Router router_;
    WebSocketRouter ws_router_;

    static constexpr int BUFFER_SIZE = 4096;
};
