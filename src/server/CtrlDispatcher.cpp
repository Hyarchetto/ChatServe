// CtrlDispatcher — 中控实现 分层单向解耦 业务委托 AppRouter+RoomManager
#include "server/CtrlDispatcher.h"

#include <iostream>
#include <stdexcept>
#include <utility>

#include "app/AppParser.h"
#include "app/AppMessage.h"

CtrlDispatcher::CtrlDispatcher(ThreadPool& works) : works_(works), app_router_(room_mgr_) {
    // 两个邮箱的回调构造即绑定 唤醒 fd 留到 start 里注册
    this->uplink_box_ = std::make_unique<UplinkBox>(
        [this](std::vector<CtrlUp>& ups) { this->handle_uplink(ups); });
    this->result_box_ = std::make_unique<ResultBox>(
        [this](std::vector<CtrlResult>& results) { this->handle_result(results); });
}

CtrlDispatcher::~CtrlDispatcher() {
    this->stop();
}

// 挂接第 io 个 io worker 的下行邮箱 序号须从 0 起连续无重复 须在 start 前完成
// 跳号或重复挂接是装配错误 当场抛 表长即挂接次数 表内不含空指针
void CtrlDispatcher::attach_downlink_box(size_t io, DownlinkBox& box) {
    if (io != this->downlink_boxes_.size()) {
        throw std::logic_error("io 下行邮箱挂接序号不连续");
    }
    this->downlink_boxes_.push_back(&box);
}

bool CtrlDispatcher::start() {
    if (this->started_) {
        return true;
    }
    if (!this->loop_.init()) {
        return false;
    }
    // 两个邮箱要先于线程把自己的唤醒 fd 注册进来
    if (!this->uplink_box_->attach(this->loop_) ||
        !this->result_box_->attach(this->loop_)) {
        return false;
    }
    this->started_ = true;
    this->thread_ = std::thread([this]() { this->loop_.loop(); });
    return true;
}

void CtrlDispatcher::stop() {
    this->request_stop();
    if (this->thread_.joinable()) {
        this->thread_.join();
    }
}

void CtrlDispatcher::request_stop() {
    this->loop_.quit();
}

// ==================== 上行邮箱 只在中控线程 ====================
// 一轮拿到整批 本轮要投池的命令先攒在 cmds 里 循环走完一次投进去
// 逐条 post 的话每一条都要抢一次池的队列锁唤醒一次 worker 批量越大亏得越多
void CtrlDispatcher::handle_uplink(std::vector<CtrlUp>& ups) {
    std::vector<Cmd> cmds;
    cmds.reserve(ups.size());
    for (auto& up : ups) {
        switch (up.kind_) {
            case CtrlUpKind::WS_TEXT:
            case CtrlUpKind::WS_BINARY: {
                Cmd cmd;
                cmd.sess_ = std::move(up.sess_);
                cmd.kind_ = up.kind_;
                cmd.data_ = std::move(up.text_);
                this->enqueue_cmd(std::move(cmd), cmds);
                break;
            }
            case CtrlUpKind::CLOSED: {
                this->handle_close(std::move(up.sess_), cmds);
                break;
            }
        }
    }
    this->submit_cmds(std::move(cmds));
}

// 连接关闭 队列里那些命令对已死的连接没有意义 整批丢掉
// 有在途业务就只登记 等它跑完由 advance_lane 接着收尾 没有则当场收尾
void CtrlDispatcher::handle_close(std::shared_ptr<Session> sess, std::vector<Cmd>& cmds) {
    Session* key = sess.get();
    auto [it, inserted] = this->lanes_.try_emplace(key);
    it->second.closing_ = true;
    if (!inserted) {
        it->second.pending_.clear();
        return;
    }
    it->second.sess_ = std::move(sess);
    this->advance_lane(key, cmds);
}

// ==================== 单飞门 只在中控线程 ====================
// 该 Session 已有操作在途则入队 否则当即开跑并攒进 cmds 待本轮一次投池
void CtrlDispatcher::enqueue_cmd(Cmd cmd, std::vector<Cmd>& cmds) {
    Session* key = cmd.sess_.get();
    auto [it, inserted] = this->lanes_.try_emplace(key);
    if (!inserted) {
        it->second.pending_.push_back(std::move(cmd));
        return;
    }
    it->second.sess_ = cmd.sess_;    // 车道持有会话 拷贝在前 Cmd 那份随后移走
    cmds.push_back(std::move(cmd));  // 新车道 这条命令当即开跑
}

// 一条操作结束时推进一步 收尾优先于排队 两者都没有则撤销登记
// 有在途操作就有车道 键必在表中
void CtrlDispatcher::advance_lane(Session* key, std::vector<Cmd>& cmds) {
    auto it = this->lanes_.find(key);
    Lane& lane = it->second;
    if (lane.closing_) {
        lane.closing_ = false;  // 排定即清 不清收尾会反复排
        cmds.push_back(Cmd{lane.sess_, CtrlUpKind::CLOSED, {}});
        return;
    }
    // 如果 pending_ 为空，说明代办业务已完全处理
    if (lane.pending_.empty()) {
        // 清理并返回
        this->lanes_.erase(it);
        return;
    }
    cmds.push_back(std::move(lane.pending_.front()));
    lane.pending_.pop_front();
}

// 一次把攒下的命令推进池 调用方须已在 lanes_ 中为这些 Session 占好位
void CtrlDispatcher::submit_cmds(std::vector<Cmd> cmds) {
    if (cmds.empty()) {
        return;
    }
    // key 先取 提交失败时 cmds 已被移入 取不到
    std::vector<Session*> keys;
    std::vector<std::function<void()>> tasks;
    keys.reserve(cmds.size());
    tasks.reserve(cmds.size());
    for (auto& cmd : cmds) {
        keys.push_back(cmd.sess_.get());
        tasks.push_back([this, cmd = std::move(cmd)]() mutable {
            this->run_business(std::move(cmd));
        });
    }
    try {
        this->works_.post_batch(std::move(tasks));
    }
    catch (const std::exception& e) {
        std::cerr << "中控提交业务失败 " << e.what() << std::endl;
        // 这批会话的通道废了 车道一并撤掉 不留没人推进的队列
        for (Session* key : keys) {
            this->lanes_.erase(key);
        }
    }
}

// 回程邮箱回调只在中控线程执行 一轮拿到整批响应
// 整批的帧合成一串一次分拣一次投放 整批的推进结果一次投池
void CtrlDispatcher::handle_result(std::vector<CtrlResult>& results) {
    std::vector<CtrlDown> frames;
    std::vector<Cmd> cmds;
    for (auto& r : results) {
        for (auto& f : r.frames_) {
            frames.push_back(std::move(f));
        }
        this->advance_lane(r.sess_.get(), cmds);
    }
    this->dispatch(std::move(frames));
    this->submit_cmds(std::move(cmds));
}

// 业务池线程入口 只按 Session 算响应 不碰会话容器与 io 邮箱
void CtrlDispatcher::run_business(Cmd cmd) {
    std::shared_ptr<Session> sess = std::move(cmd.sess_);
    std::vector<CtrlDown> frames;
    try {
        switch (cmd.kind_) {
            case CtrlUpKind::WS_TEXT:
                frames = this->route(sess, cmd.data_);
                break;
            case CtrlUpKind::WS_BINARY:
                frames = this->route_chunk(sess, cmd.data_);
                break;
            case CtrlUpKind::CLOSED:
                frames = this->app_router_.cleanup(sess);
                break;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "业务处理异常 fd=" << sess->fd_ << " " << e.what() << std::endl;
    }
    // 交还中控线程 中控按轮攒批分拣 携带 Session 保活
    this->result_box_->post(CtrlResult{std::move(sess), std::move(frames)});
}

// 路由一条文本命令 委托 AppRouter 执行业务
std::vector<CtrlDown> CtrlDispatcher::route(std::shared_ptr<Session> sess,
                                            const std::string& text) {
    std::vector<CtrlDown> frames;
    if (!sess->alive_) {
        return frames;  // io 已关 弃处理 收尾另有 CLEANUP 一条
    }
    AppMessage msg = AppParser::parse(text);
    return this->app_router_.handle(std::move(sess), msg);
}

// 处理一个二进制分块 委托 AppRouter 转发给下载方
std::vector<CtrlDown> CtrlDispatcher::route_chunk(std::shared_ptr<Session> sess,
                                                  const std::string& data) {
    std::vector<CtrlDown> frames;
    if (!sess->alive_) {
        return frames;  // io 已关 弃处理 收尾另有 CLEANUP 一条
    }
    return this->app_router_.handle_chunk(std::move(sess), data);
}

// ==================== 唯一分发点 只在中控线程 ====================
void CtrlDispatcher::dispatch(std::vector<CtrlDown> frames) {
    if (frames.empty()) {
        return;
    }
    // 按帧目标会话自带的归属 io 攒批 每 io 一次唤醒 无表可查
    std::vector<std::vector<CtrlDown>> grouped(this->downlink_boxes_.size());
    for (auto& f : frames) {
        const int io = f.sess_->io_;
        if (!f.sess_->alive_) {
            continue;  // 目标已关闭 弃帧
        }
        grouped[static_cast<size_t>(io)].push_back(std::move(f));
    }
    for (size_t i = 0; i < grouped.size(); ++i) {
        if (!grouped[i].empty()) {
            this->downlink_boxes_[i]->post_batch(std::move(grouped[i]));
        }
    }
}
