// CtrlDispatcher — 中控实现 分层单向解耦 业务委托 AppRouter+RoomManager
#include "server/CtrlDispatcher.h"

#include <iostream>
#include <utility>

#include "app/AppParser.h"
#include "app/AppMessage.h"

CtrlDispatcher::CtrlDispatcher(ThreadPool& works) : works_(works), app_router_(room_mgr_) {
    // 收件箱 sink 只经中控 loop 触发 构造即绑定 线程启动前投递先进队列
    this->inbox_ = std::make_unique<Inbox>(this->loop_,
        [this](CtrlUp up) { this->handle_uplink(std::move(up)); });
}

CtrlDispatcher::~CtrlDispatcher() {
    this->stop();
}

// 出站表按需增长到能容纳第 io 个 io worker 须在 start 前完成
void CtrlDispatcher::attach_outbox(size_t io, Outbox& box) {
    if (io >= this->outboxes_.size()) {
        this->outboxes_.resize(io + 1, nullptr);
    }
    this->outboxes_[io] = &box;
}

bool CtrlDispatcher::start() {
    if (this->started_) {
        return true;
    }
    if (!this->loop_.init()) {
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

// ==================== 收件箱 只在中控线程 ====================
void CtrlDispatcher::handle_uplink(CtrlUp up) {
    switch (up.kind_) {
        case CtrlUpKind::WS_TEXT:
        case CtrlUpKind::WS_BINARY: {
            Session* key = up.sess_.get();
            Cmd cmd;
            cmd.sess_ = std::move(up.sess_);
            cmd.binary_ = (up.kind_ == CtrlUpKind::WS_BINARY);
            cmd.data_ = std::move(up.text_);
            // 单飞门 该 Session 已有业务在途则入队 否则置在途并提交
            if (this->running_.count(key)) {
                this->pending_[key].push_back(std::move(cmd));
                break;
            }
            this->running_.insert(key);
            this->submit_cmd(std::move(cmd));
            break;
        }
        case CtrlUpKind::CLOSED: {
            this->cleanup(std::move(up.sess_));
            break;
        }
    }
}

// ==================== 单飞门 只在中控线程 ====================
void CtrlDispatcher::submit_cmd(Cmd cmd) {
    // key 先取 提交失败时 cmd 已被移入 取不到
    Session* key = cmd.sess_.get();
    try {
        this->works_.submit([this, cmd = std::move(cmd)]() mutable {
            this->run_business(std::move(cmd));
        });
    }
    catch (const std::exception& e) {
        std::cerr << "中控提交业务失败 " << e.what() << std::endl;
        this->running_.erase(key);
    }
}

void CtrlDispatcher::advance_lane(Session* key) {
    auto it = this->pending_.find(key);
    if (it == this->pending_.end() || it->second.empty()) {
        if (it != this->pending_.end()) {
            this->pending_.erase(it);
        }
        this->running_.erase(key);
        return;
    }
    Cmd cmd = std::move(it->second.front());
    it->second.pop_front();
    this->submit_cmd(std::move(cmd));
}

// 业务完成回中控线程 先做唯一分发 再推进该 Session 单飞门
void CtrlDispatcher::on_done(std::shared_ptr<Session> sess, std::vector<CtrlDown> frames) {
    Session* key = sess.get();  // sess 持引用 期间对象必存活 指针稳定
    this->dispatch(std::move(frames));
    this->advance_lane(key);
}

// 业务池线程入口 只按 Session 算响应 不碰会话容器与 io 频道
void CtrlDispatcher::run_business(Cmd cmd) {
    std::vector<CtrlDown> frames;
    try {
        if (cmd.binary_) {
            frames = this->route_chunk(cmd.sess_, std::move(cmd.data_));
        }
        else {
            frames = this->route(cmd.sess_, cmd.data_);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "业务处理异常 fd=" << cmd.sess_->fd_ << " " << e.what() << std::endl;
    }
    // 整批交还中控线程 由中控分发到各 io 频道 携带 Session 保活
    this->loop_.post([this, sess = std::move(cmd.sess_),
                      frames = std::move(frames)]() mutable {
        this->on_done(std::move(sess), std::move(frames));
    });
}

// 路由一条命令 委托 AppRouter 执行业务 持业务串行门
std::vector<CtrlDown> CtrlDispatcher::route(std::shared_ptr<Session> sess,
                                            const std::string& text) {
    std::vector<CtrlDown> frames;
    if (!sess->alive_) {
        return frames;  // io 已关 弃处理
    }
    AppMessage msg = AppParser::parse(text);
    std::unique_lock<std::shared_mutex> lock(this->mtx_);
    return this->app_router_.handle(std::move(sess), msg);
}

// 处理一个二进制分块 持业务锁执行
std::vector<CtrlDown> CtrlDispatcher::route_chunk(std::shared_ptr<Session> sess,
                                                  const std::string& data) {
    std::vector<CtrlDown> frames;
    if (!sess->alive_) {
        return frames;  // io 已关 弃处理
    }
    std::unique_lock<std::shared_mutex> lock(this->mtx_);
    return this->app_router_.handle_chunk(std::move(sess), data);
}

// 连接清理 只中控线程调用 回收单飞门 委托业务清理
void CtrlDispatcher::cleanup(std::shared_ptr<Session> sess) {
    Session* key = sess.get();
    this->pending_.erase(key);
    this->running_.erase(key);

    std::vector<CtrlDown> frames;
    {
        std::unique_lock<std::shared_mutex> lock(this->mtx_);
        frames = this->app_router_.cleanup(std::move(sess));
    }
    this->dispatch(std::move(frames));
}

// ==================== 唯一分发点 只在中控线程 ====================
void CtrlDispatcher::dispatch(std::vector<CtrlDown> frames) {
    if (frames.empty()) {
        return;
    }
    // 按帧目标会话自带的归属 io 攒批 每 io 一次唤醒 无表可查
    std::vector<std::vector<CtrlDown>> grouped(this->outboxes_.size());
    for (auto& f : frames) {
        const int io = f.sess_->io_;
        if (!f.sess_->alive_) {
            continue;  // 目标已关闭 弃帧
        }
        grouped[static_cast<size_t>(io)].push_back(std::move(f));
    }
    for (size_t i = 0; i < grouped.size(); ++i) {
        if (!grouped[i].empty()) {
            this->outboxes_[i]->post_batch(std::move(grouped[i]));
        }
    }
}
