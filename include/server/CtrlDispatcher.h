// CtrlDispatcher — 中控 一条线程做唯一分发 并串行调度业务
// 分层单向解耦：
//   io 线程只管连接与协议层的出入 剥完帧用 Session 控制块上报 排空自己邮箱写自己连接
//   中控是两者之间唯一的一跳 归属 io 随 Session 自带 分发时直接读 放进对应 io 邮箱并唤醒
//   业务线程池不知道 io 线程存在 只按 Session 控制块算响应 整批交还中控线程
// 业务逻辑集中在 AppRouter 命令表 与 RoomManager 房间
// 单飞门保证同一 Session 至多一条命令在池 顺序即上报序 房间与传输状态各自的锁保护
// 连接关闭也走这道门 收尾排在在途业务之后 队列里那些对死连接没意义的命令直接丢
//
// 两个方向都攒批 交界的次数按批算不按条算
//   上行 一轮排空攒出整批命令 末尾一次投进池 不逐条抢池的队列锁
//   回程 走 result_box_ 池线程算完投回来 中控一轮拿到整批响应 合起来分拣一次投一次 io

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../ctrl/Session.h"
#include "../ctrl/CtrlMsg.h"
#include "../core/Mailbox.h"
#include "../chatroom/Room.h"
#include "../app/AppRouter.h"
#include "../core/EventLoop.h"
#include "../core/ThreadPool.h"

class CtrlDispatcher {
public:
    using UplinkBox = Mailbox<CtrlUp>;              // io 投 中控收
    using DownlinkBox = Mailbox<CtrlDown>;          // 中控投 io 收

    // works 为业务工作线程池 归工厂持有 命令在其线程执行
    explicit CtrlDispatcher(ThreadPool& works);
    ~CtrlDispatcher();

    CtrlDispatcher(const CtrlDispatcher&) = delete;
    CtrlDispatcher& operator=(const CtrlDispatcher&) = delete;

    // 本中控的上行邮箱 即各 io worker 的上报口 装配时注入
    UplinkBox& uplink_box() { return *this->uplink_box_; }

    // 把第 io 个 io worker 的下行邮箱接进来 序号须从 0 起连续无重复 须在 start 前完成
    void attach_downlink_box(size_t io, DownlinkBox& box);

    // 起中控线程跑事件循环 返回 false 表示初始化失败
    bool start();
    // 停事件循环并回收线程
    void stop();
    // 只置退出标志不阻塞 供信号处理器调用 线程收尾由 stop 完成
    void request_stop();

private:
    // 一条待处理的操作 携带会话保活
    // kind_ 直接复用上行事件类型 投池路上不再重新编码
    // CLOSED 在此表示连接关闭后的业务收尾 它只可能由那条 CLOSED 事件产生
    struct Cmd {
        std::shared_ptr<Session> sess_;             // 会话 持引用让队列中的待办不被释放
        CtrlUpKind kind_ = CtrlUpKind::WS_TEXT;
        std::string data_;                          // 应用原文或分块原始字节 收尾时为空
    };

    // 一条算完的业务响应 携带会话保活
    struct CtrlResult {
        std::shared_ptr<Session> sess_;
        std::vector<CtrlDown> frames_;
    };

    using ResultBox = Mailbox<CtrlResult>;          // 池投 中控收

    // 一条车道的全部状态 车道即一条会话的串行通道
    // 有键即有命令在途 键消失即车道空闲 键只作身份 会话由值持有到那一刻
    struct Lane {
        std::shared_ptr<Session> sess_;   // 车道持有会话
        std::deque<Cmd> pending_;         // 排队等跑的普通命令
        bool closing_ = false;            // 欠一条收尾 排定后清掉
    };

    // 上行邮箱回调只在中控线程执行 一轮拿到整批
    void handle_uplink(std::vector<CtrlUp>& ups);
    // 回程邮箱回调只在中控线程执行 一轮拿到整批响应
    // 整批的帧合起来分拣一次 整批的推进结果一次投池
    void handle_result(std::vector<CtrlResult>& results);
    // 连接关闭 只中控线程执行 登记待收尾并把要收尾的命令攒进 cmds
    void handle_close(std::shared_ptr<Session> sess, std::vector<Cmd>& cmds);

    // 单飞门入口 该 Session 已有操作在途则入队 否则当即开跑并攒进 cmds
    void enqueue_cmd(Cmd cmd, std::vector<Cmd>& cmds);
    // 单飞门推进 只中控线程调用 有待收尾则优先 否则拉下一条 结果攒进 cmds
    void advance_lane(Session* key, std::vector<Cmd>& cmds);
    // 把攒下的一批命令一次投进业务池
    void submit_cmds(std::vector<Cmd> cmds);
    // 业务池线程入口 按 kind 委托 AppRouter 算响应
    void run_business(Cmd cmd);
    // 路由一条文本命令
    std::vector<CtrlDown> route(std::shared_ptr<Session> sess, const std::string& text);
    // 处理一个二进制分块
    std::vector<CtrlDown> route_chunk(std::shared_ptr<Session> sess, const std::string& data);

    // 唯一分发点 只中控线程调用 按帧目标会话自带的归属 io 放进对应邮箱
    void dispatch(std::vector<CtrlDown> frames);

    ThreadPool& works_;                             // 业务线程池 工厂持有
    EventLoop loop_;
    std::unique_ptr<UplinkBox> uplink_box_;         // io→中控
    std::unique_ptr<ResultBox> result_box_;         // 池→中控
    std::vector<DownlinkBox*> downlink_boxes_;      // 中控→io
    std::thread thread_;
    bool started_ = false;

    // 中控线程专用 无锁 登记与撤销都只经 enqueue_cmd 与 advance_lane 两处
    std::unordered_map<Session*, Lane> lanes_;      // 会话 → 车道状态

    // 业务层 复用已验证算法 房间/传输/命令逻辑不在此重复
    // room_mgr_ 必须排在 app_router_ 之前 后者构造时按引用绑定它 声明序即析构逆序
    RoomManager room_mgr_;                          // 房间与房间内传输管理器
    AppRouter app_router_;                          // 命令表与业务 handler 持 room_mgr_ 引用
};
