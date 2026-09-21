// 应用层命令路由 — WebRTC 信令中继域
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "app/AppRouter.h"
#include "app/AppParser.h"
#include "ctrl/Session.h"

// 解析 target_fd 并把 payload 转发给同房间的目标连接
// 帧里把发送方换成自己的 fd 目标不在房间则不发
std::vector<CtrlDown> AppRouter::relay_signal(std::shared_ptr<Session> sess, const AppMessage& msg,
                                              const std::string& command, size_t payload_count) {
    std::vector<CtrlDown> results;
    if (msg.param_count() < 1 + payload_count) {
        return results;
    }
    int target_fd = 0;
    try {
        target_fd = std::stoi(msg.param(0));
    }
    catch (const std::exception& e) {
        std::cerr << command << " parse target_fd failed: " << e.what()
                  << " for param '" << msg.param(0) << "'" << std::endl;
        return results;
    }
    // 未加入房间则房间查不到 转发自然空转
    auto room = this->room_mgr_.find_room(sess->room_);
    auto target = find_peer(room, target_fd);
    if (!target) {
        return results;
    }
    std::vector<std::string> params;
    params.reserve(1 + payload_count);
    params.push_back(std::to_string(sess->fd_));
    for (size_t i = 0; i < payload_count; ++i) {
        params.push_back(msg.param(1 + i));
    }
    results.push_back({target, AppParser::build_frame(command, params)});
    return results;
}

void AppRouter::register_signalling() {
    // WebRTC 信令转发 — 客户端发 COMMAND|target_fd|payload 服务端转 COMMAND|from_fd|payload
    // payload 是 base64 的 SDP 或候选地址 服务器不看内容只转发
    // OFFER/ANSWER/ICE 各带一个 payload MEDIA 带 kind 与 state 两个
    // 媒体流走浏览器 P2P 直连 服务器只转发信令文本

    this->on("OFFER", [this](std::shared_ptr<Session> sess, const AppMessage& msg) {
        return this->relay_signal(std::move(sess), msg, "OFFER", 1);
    });
    this->on("ANSWER", [this](std::shared_ptr<Session> sess, const AppMessage& msg) {
        return this->relay_signal(std::move(sess), msg, "ANSWER", 1);
    });
    this->on("ICE", [this](std::shared_ptr<Session> sess, const AppMessage& msg) {
        return this->relay_signal(std::move(sess), msg, "ICE", 1);
    });
    this->on("MEDIA", [this](std::shared_ptr<Session> sess, const AppMessage& msg) {
        return this->relay_signal(std::move(sess), msg, "MEDIA", 2);
    });
}
