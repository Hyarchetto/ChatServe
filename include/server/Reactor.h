// 单 Reactor 多线程 TCP 服务器
// 所有连接在同一个 EventLoop 里管着，耗时的活直接丢线程池
// HTTP → HttpParser → HttpRouter → HttpResponse → 回复
// WebSocket → WsParser → WsAppRouter → 聊天室路由 → 回复
// BINARY 帧 → TransferManager → 滑动窗口转发
#pragma once

#include <functional>
#include <memory>
#include <cstdint>

#include "Connection.h"
#include "ConnRegistry.h"
#include "WriteScheduler.h"
#include "Acceptor.h"
#include "HttpHandler.h"
#include "WsHandler.h"
#include "../core/ThreadPool.h"
#include "../core/EventLoop.h"
#include "../http/HttpRouter.h"
#include "../ws/WsAppRouter.h"
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

    // 信号安全的轻量唤醒 只叫醒事件循环 线程池收尾交给 stop 完成
    void request_stop();

    // 添加一个客户端连接到本 Reactor
    // 必须在 IO 线程调用，直接注册到 EventLoop
    void add_connection(int fd);

    // 唯一的连接关闭入口 通用销毁 + 按 ws_mode_ 分发协议清理
    // 必须在 IO 线程调用
    void close_connection(const std::shared_ptr<Connection>& conn);

private:
    // ==================== 核心组件 ====================
    EventLoop loop_;
    ThreadPool works_;
    TransferManager transfer_mgr_;
    RoomManager room_mgr_;

    // ==================== 路由 ====================
    HttpRouter http_router_;
    WsAppRouter ws_app_router_;
    // ==================== 连接管理 ====================
    ConnRegistry conn_registry_;
    // ==================== 写调度 ====================
    WriteScheduler writer_;
    // ==================== Acceptor ====================
    Acceptor acceptor_;
    // ==================== 协议处理 ====================
    // 依赖以上所有组件 声明顺序保证构造时引用已就绪
    HttpHandler http_handler_;
    WsHandler ws_handler_;

    // ==================== 方法 ====================
    // IO 处理
    bool read_data(const std::shared_ptr<Connection>& conn);
    void handle_clientfd(const std::shared_ptr<Connection>& conn);

    // 协议无关的连接销毁 只被 close_connection 调用
    void del_connection(const std::shared_ptr<Connection>& conn);

    static constexpr int BUFFER_SIZE = 4096;
};
