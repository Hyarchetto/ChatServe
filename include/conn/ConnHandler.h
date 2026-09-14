// ConnHandler — io worker 连接泵 一份归属一个从属 Reactor io 线程
// 职责边界 连接泵 + 协议层 业务全部经共享 Session 控制块上行中控
// HTTP/静态文件内联处理 WebSocket 只做分帧 只写给本线程的连接
// 完整 TEXT 应用消息上行中控 断开或 CLOSE 上报 中控据此清理业务
// 中控下行经 outbox 邮箱到达本线程 按 Session 找连接逐条写出
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Connection.h"
#include "WriteScheduler.h"
#include "../core/EventLoop.h"
#include "../ctrl/Session.h"
#include "../ctrl/CtrlMsg.h"
#include "../ctrl/Mailbox.h"
#include "../http/HttpHandler.h"
#include "../ws/WsHandler.h"

class ConnHandler {
public:
    // 共享上行收件箱与 io 序号由装配注入 事件循环本类持有
    ConnHandler(EventLoop& loop, int io_index,
                Mailbox<CtrlUp>& ctrl_inbox);

    // 添加一个客户端连接到本处理器
    // 必须在 io 线程调用 直接注册到 EventLoop
    void add_connection(int fd);

    // 中控下行邮箱 装配阶段由中控 attach
    Mailbox<CtrlDown>& outbox() { return this->outbox_; }

    // 唯一的连接关闭入口 幂等 断开或写调度发完 CLOSE 后触发
    // 必须在 io 线程调用
    void close_connection(const std::shared_ptr<Connection>& conn);

private:
    // 从连接读入读缓冲 返回 false 表示连接已关闭
    bool pump_read(const std::shared_ptr<Connection>& conn);
    // 客户端数据总入口 按 ws_mode_ 分流 HTTP 或 WS
    void handle_client_fd(const std::shared_ptr<Connection>& conn);
    // 施加一条 HTTP 决策到连接 与 handle_ws 对称 升级握手在此完成
    void handle_http(const std::shared_ptr<Connection>& conn, HttpAction action);
    // 施加一条 WS 决策到连接 与 handle_http 对称 上行与回包在此发生
    void handle_ws(const std::shared_ptr<Connection>& conn, WsAction action);
    // 上行一条事件到中控 携带共享 Session 控制块
    void uplink(CtrlUp up);
    // 上行一批应用消息 文本或二进制分块 按入队序逐条上报
    void uplink_messages(const std::shared_ptr<Connection>& conn,
                         std::vector<std::string> items, bool binary);
    // 下行邮箱 sink 只在本 io 线程执行
    void downlink(CtrlDown down);

    EventLoop& loop_;
    int io_ = 0;                      // 本 worker 的 io 序号 建连接时写进 Session 供中控分发
    WriteScheduler writer_;
    HttpHandler http_;
    WsHandler ws_;
    Mailbox<CtrlDown> outbox_;
    Mailbox<CtrlUp>& ctrl_inbox_;
    std::unordered_map<Session*, std::shared_ptr<Connection>> conns_;  // Session → 连接 下行查找用

    static constexpr int kBufferSize = 4096;
};
