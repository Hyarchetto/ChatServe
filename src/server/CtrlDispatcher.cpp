// CtrlDispatcher — 中控实现，分层单向解耦，业务委托 AppHandler
#include "server/CtrlDispatcher.h"

#include <stdexcept>
#include <utility>

CtrlDispatcher::CtrlDispatcher(AppHandler& app) : app_handler_(app) {
    // 两个邮箱的回调构造即绑定，唤醒 fd 留到 start 里注册
    this->uplink_box_ = std::make_unique<UplinkBox>(
        [this](std::vector<CtrlUp>& ups) { this->handle_uplink(ups); });
    this->result_box_ = std::make_unique<ResultBox>(
        [this](std::vector<CtrlResult>& results) { this->handle_result(results); });
    // 邮箱已建、回调已绑，此时把引用交给业务处理段，早于 start
    this->app_handler_.attach_result_box(*this->result_box_);
}

CtrlDispatcher::~CtrlDispatcher() {
    this->stop();
}

// 挂接第 io 个 io worker 的下行邮箱，序号须从 0 起连续无重复，须在 start 前完成
// 跳号或重复挂接是装配错误，当场抛，表长即挂接次数，表内不含空指针
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

// ==================== 上行邮箱，只在中控线程 ====================
// 一轮拿到整批，本轮要投池的命令先攒在 cmds 里，循环走完一次投进去
// 逐条 post 的话每一条都要抢一次池的队列锁唤醒一次 worker 批量越大亏得越多
void CtrlDispatcher::handle_uplink(std::vector<CtrlUp>& ups) {
    std::vector<CtrlCmd> cmds;
    cmds.reserve(ups.size());
    for (auto& up : ups) {
        switch (up.kind_) {
            case CtrlUpKind::WS_TEXT:
            case CtrlUpKind::WS_BINARY: {
                CtrlCmd cmd;
                cmd.sess_ = std::move(up.sess_);
                cmd.kind_ = up.kind_;
                cmd.text_ = std::move(up.text_);
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

// 连接关闭，队列里那些命令对已死的连接没有意义，整批丢掉
// 有在途业务就只登记，等它跑完由 advance_lane 接着收尾，没有则当场收尾
void CtrlDispatcher::handle_close(std::shared_ptr<Session> sess, std::vector<CtrlCmd>& cmds) {
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

// ==================== 单飞门，只在中控线程 ====================
// 该 Session 已有操作在途则入队，否则当即开跑并攒进 cmds 待本轮一次投池
void CtrlDispatcher::enqueue_cmd(CtrlCmd cmd, std::vector<CtrlCmd>& cmds) {
    Session* key = cmd.sess_.get();
    auto [it, inserted] = this->lanes_.try_emplace(key);
    if (!inserted) {
        it->second.pending_.push_back(std::move(cmd));
        return;
    }
    it->second.sess_ = cmd.sess_;    // 车道持有会话，拷贝在前 CtrlCmd 那份随后移走
    cmds.push_back(std::move(cmd));  // 新车道，这条命令当即开跑
}

// 一条操作结束时推进一步，收尾优先于排队，两者都没有则撤销登记
// 有在途操作就有车道，键必在表中
void CtrlDispatcher::advance_lane(Session* key, std::vector<CtrlCmd>& cmds) {
    auto it = this->lanes_.find(key);
    Lane& lane = it->second;
    if (lane.closing_) {
        lane.closing_ = false;  // 排定即清，不清收尾会反复排
        cmds.push_back(CtrlCmd{lane.sess_, CtrlUpKind::CLOSED, {}});
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

// 一次把攒下的命令交给业务处理段投池，调用方须已在 lanes_ 中为这些 Session 占好位
void CtrlDispatcher::submit_cmds(std::vector<CtrlCmd> cmds) {
    if (cmds.empty()) {
        return;
    }
    // 投池失败的会话通道作废，车道一并撤掉
    for (Session* key : this->app_handler_.handle(std::move(cmds))) {
        this->lanes_.erase(key);
    }
}

// 回程邮箱回调只在中控线程执行，一轮拿到整批响应
// 整批的帧合成一串一次分拣一次投放，整批的推进结果一次交给业务处理段
void CtrlDispatcher::handle_result(std::vector<CtrlResult>& results) {
    std::vector<CtrlDown> frames;
    std::vector<CtrlCmd> cmds;
    for (auto& r : results) {
        for (auto& f : r.frames_) {
            frames.push_back(std::move(f));
        }
        this->advance_lane(r.sess_.get(), cmds);
    }
    this->dispatch(std::move(frames));
    this->submit_cmds(std::move(cmds));
}

// ==================== 唯一分发点，只在中控线程 ====================
void CtrlDispatcher::dispatch(std::vector<CtrlDown> frames) {
    if (frames.empty()) {
        return;
    }
    // 按帧目标会话自带的归属 io 攒批，每 io 一次唤醒，无表可查
    std::vector<std::vector<CtrlDown>> grouped(this->downlink_boxes_.size());
    for (auto& f : frames) {
        const int io = f.sess_->io_;
        if (!f.sess_->alive_) {
            continue;  // 目标已关闭，弃帧
        }
        grouped[static_cast<size_t>(io)].push_back(std::move(f));
    }
    for (size_t i = 0; i < grouped.size(); ++i) {
        if (!grouped[i].empty()) {
            this->downlink_boxes_[i]->post_batch(std::move(grouped[i]));
        }
    }
}
