// ConnHandler — io worker 连接泵 一份归属一个从属 Reactor io 线程
// 职责边界 连接泵 + 协议层 业务全部经共享 Session 控制块上行中控
// HTTP/静态文件内联处理 WebSocket 只做分帧 只写给本线程的连接
// 完整 TEXT 应用消息上行中控 断开或 CLOSE 上报 中控据此清理业务
// 中控下行经下行邮箱到达本线程 按 Session 找连接逐条写出
#pragma once

#include <memory>
#include <chrono>
#include <unordered_map>
#include <vector>

#include "Connection.h"
#include "WriteScheduler.h"
#include "../core/EventLoop.h"
#include "../core/Timer.h"
#include "../ctrl/Session.h"
#include "../ctrl/CtrlMsg.h"
#include "../core/Mailbox.h"
#include "../http/HttpHandler.h"
#include "../ws/WsHandler.h"

class ConnHandler {
public:
    // 共享上行邮箱与 io 序号由装配注入 事件循环本类持有
    ConnHandler(EventLoop& loop, int io_index, Mailbox<CtrlUp>& ctrl_uplink_box);

    // 装配到事件循环 把本处理器要注册的 fd 都挂上
    // 须在 loop.init() 之后 loop.loop() 之前调用
    bool attach();

    // 添加一个客户端连接到本处理器
    // 必须在 io 线程调用 直接注册到 EventLoop
    void add_connection(int fd);

    // 本 io 的下行邮箱 装配阶段由中控 attach
    Mailbox<CtrlDown>& downlink_box() { return this->downlink_box_; }

    // 唯一的连接关闭入口 幂等 断开或写调度发完 CLOSE 后触发
    // 必须在 io 线程调用
    void close_connection(const std::shared_ptr<Connection>& conn);

    // 心跳节拍 取当前时刻扫一遍全部连接 必须在 io 线程调用
    void on_tick();
    // 按注入的时刻扫描 入站空闲的发 PING 出入站都空闲的判死
    // 时刻由调用方给 测试据此确定性地驱动各分支
    void on_tick(std::chrono::steady_clock::time_point now);

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
    // 上行一条 WS 决策里的全部应用消息 文本与二进制合成一批交给中控
    void uplink_ws(const std::shared_ptr<Connection>& conn, WsAction& action);
    // 下行邮箱回调只在本 io 线程执行 一次拿到整批 逐条 move 走内容
    void downlink_batch(std::vector<CtrlDown>& downs);

    EventLoop& loop_;                   // 本线程事件循环 writer_ 与 downlink_box_ 按引用绑定它
    int io_ = 0;                        // 本 worker 的 io 序号
    WriteScheduler writer_;
    HttpHandler http_;
    WsHandler ws_;
    Timer heartbeat_;                   // 本线程的扫描节拍 主形态不建
    Mailbox<CtrlDown> downlink_box_;    // 中控投 本线程收
    Mailbox<CtrlUp>& ctrl_uplink_box_;  // 本线程投 中控收
    // 键是身份 值是所有权 登记由 add_connection 撤销由 close_connection 两处收口
    // 下行按 Session 查找 心跳是整表遍历所以先按值快照
    std::unordered_map<Session*, std::shared_ptr<Connection>> conns_;

    static constexpr int kBufferSize = 4096;
};
