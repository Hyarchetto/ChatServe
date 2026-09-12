// CtrlDispatcher — 中控 一条线程做唯一分发 并串行调度业务
// 分层单向解耦：
//   业务线程池不知道 io 线程存在 只按 Session 控制块算响应 整批交还中控线程
//   中控不持会话表 归属 io 随 Session 自带 分发时直接读 放进对应 io 频道并唤醒
//   io 线程只管 socket 生命周期 用 Connection->session 上报 排空自己频道写自己连接
// 业务逻辑集中在 AppRouter(命令表) + RoomManager(房间) 均复用已验证算法
// 命令经单飞门(同一 Session 至多一条在池)保证顺序 业务持单把锁串行房间操作
#pragma once

#include <deque>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../ctrl/Session.h"
#include "../ctrl/CtrlMsg.h"
#include "../ctrl/Mailbox.h"
#include "../chatroom/Room.h"
#include "../app/AppRouter.h"
#include "../core/EventLoop.h"
#include "../core/ThreadPool.h"

class CtrlDispatcher {
public:
    using Inbox = Mailbox<CtrlUp>;
    using Outbox = Mailbox<CtrlDown>;

    // works 为业务工作线程池 归工厂持有 命令在其线程执行
    explicit CtrlDispatcher(ThreadPool& works);
    ~CtrlDispatcher();

    CtrlDispatcher(const CtrlDispatcher&) = delete;
    CtrlDispatcher& operator=(const CtrlDispatcher&) = delete;

    // io 上行收件箱 装配时注入各 io worker
    Inbox& inbox() { return *this->inbox_; }

    // 把第 io 个出站邮箱接进来 出站表按需增长 须在 start 前完成
    void attach_outbox(size_t io, Outbox& box);

    // 起中控线程跑事件循环 返回 false 表示初始化失败
    bool start();
    // 停事件循环并回收线程
    void stop();
    // 只置退出标志不阻塞 供信号处理器调用 线程收尾由 stop 完成
    void request_stop();

private:
    // 一条待处理的上行 携带会话保活
    struct Cmd {
        std::shared_ptr<Session> sess_;     // 会话 持引用让队列中的待办不被释放
        bool binary_ = false;               // 是否为二进制分块
        std::string data_;                  // 应用原文或分块原始字节
    };

    // 收件箱 sink 只在中控线程执行
    void handle_uplink(CtrlUp up);

    // 提交一条命令进业务池 调用方须已把该 Session 标为 running
    void submit_cmd(Cmd cmd);
    // 单飞门推进 只中控线程调用 分发完成后拉下一条
    void advance_lane(Session* key);
    // 业务任务完成回中控线程 先分发再推进单飞门
    void on_done(std::shared_ptr<Session> sess, std::vector<CtrlDown> frames);
    // 业务池线程入口 委托 AppRouter 计算响应
    void run_business(Cmd cmd);
    // 路由一条文本命令 持业务锁执行
    std::vector<CtrlDown> route(std::shared_ptr<Session> sess,
                                const std::string& text);
    // 处理一个二进制分块 持业务锁执行
    std::vector<CtrlDown> route_chunk(std::shared_ptr<Session> sess,
                                      const std::string& data);
    // 连接清理 只中控线程调用 回收单飞门 委托业务清理广播
    void cleanup(std::shared_ptr<Session> sess);

    // 唯一分发点 只中控线程调用 按帧目标会话自带的归属 io 放进对应频道
    void dispatch(std::vector<CtrlDown> frames);

    ThreadPool& works_;               // 业务线程池 工厂持有
    EventLoop loop_;
    std::unique_ptr<Inbox> inbox_;    // io→中控
    std::vector<Outbox*> outboxes_;   // 中控→io 索引即 io 序号 start 前固定
    std::thread thread_;
    bool started_ = false;

    // 中控线程专用 无锁 均只在有业务在途时存在 由 cleanup 回收
    std::unordered_map<Session*, std::deque<Cmd>> pending_;              // Session → 待处理命令
    std::unordered_set<Session*> running_;                               // Session 在途标记

    // 业务层 复用已验证算法 房间/传输/命令逻辑不在此重复
    AppRouter app_router_;            // 命令表与业务 handler
    RoomManager room_mgr_;            // 房间与房间内传输管理器
    mutable std::shared_mutex mtx_;   // 业务串行门 房间与身份一致性的唯一入口
};
