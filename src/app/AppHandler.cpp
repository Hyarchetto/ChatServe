// AppHandler — 业务处理段实现
#include "app/AppHandler.h"

#include <functional>
#include <iostream>
#include <utility>

#include "app/AppMessage.h"
#include "app/AppParser.h"

AppHandler::AppHandler(ThreadPool& works) : works_(works), app_router_(room_mgr_) {}

void AppHandler::attach_result_box(ResultBox& box) {
    this->result_box_ = &box;
}

// 中控一轮投来的整批命令一次进池，逐条 post 会每条唤醒一次 worker
// 投递是全有或全无，失败时把一条都没进去的这批会话交回调用方收尾
std::vector<Session*> AppHandler::handle(std::vector<CtrlCmd> cmds) {
    std::vector<Session*> keys;
    keys.reserve(cmds.size());
    for (auto& cmd : cmds) {
        keys.push_back(cmd.sess_.get());
    }
    try {
        std::vector<std::function<void()>> tasks;
        tasks.reserve(cmds.size());
        for (auto& cmd : cmds) {
            tasks.push_back([this, cmd = std::move(cmd)]() mutable {
                this->handle_cmd(std::move(cmd));
            });
        }
        this->works_.post_batch(std::move(tasks));
    }
    catch (const std::exception& e) {
        std::cerr << "业务提交失败 " << e.what() << std::endl;
        // 将上传整批退回
        return keys;  
    }
    return {};
}

// 池线程入口，按 kind 分派，结果连同会话投回中控的结果邮箱
void AppHandler::handle_cmd(CtrlCmd cmd) {
    std::shared_ptr<Session> sess = std::move(cmd.sess_);
    std::vector<CtrlDown> frames;
    try {
        switch (cmd.kind_) {
            case CtrlUpKind::WS_TEXT:
                frames = this->route(sess, cmd.text_);
                break;
            case CtrlUpKind::WS_BINARY:
                frames = this->route_chunk(sess, std::move(cmd.text_));
                break;
            case CtrlUpKind::CLOSED:
                frames = this->app_router_.cleanup(sess);
                break;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "业务处理异常 fd=" << sess->fd_ << " " << e.what() << std::endl;
    }
    this->result_box_->post(CtrlResult{std::move(sess), std::move(frames)});
}

// 路由一条文本命令，委托 AppRouter 执行业务
std::vector<CtrlDown> AppHandler::route(std::shared_ptr<Session> sess, const std::string& text) {
    std::vector<CtrlDown> frames;
    if (!sess->alive_) {
        // io 已关，弃处理，收尾另有处理方法
        return frames;  
    }
    AppMessage msg = AppParser::parse(text);
    return this->app_router_.handle(std::move(sess), msg);
}

// 处理一个二进制分块，委托 AppRouter 转发给下载方
std::vector<CtrlDown> AppHandler::route_chunk(std::shared_ptr<Session> sess, std::string data) {
    std::vector<CtrlDown> frames;
    if (!sess->alive_) {
        return frames;  // io 已关，弃处理，收尾另有 CLOSED 一条
    }
    return this->app_router_.handle_chunk(std::move(sess), std::move(data));
}
