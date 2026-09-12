// 应用层命令路由 — WebRTC 信令中继域
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "app/AppRouter.h"
#include "app/AppParser.h"
#include "ctrl/Session.h"

// 房间内按 fd 找目标 Session 找不到返回 nullptr
static std::shared_ptr<Session> room_peer(RoomManager& room_mgr,
                                          const std::string& room_id,
                                          int target_fd) {
    auto room = room_mgr.get_or_create(room_id);
    for (auto& c : room->get_live_connections()) {
        if (c->fd_ == target_fd) {
            return c;
        }
    }
    return nullptr;
}

void AppRouter::register_signalling() {
    // ========== WebRTC 信令转发 ==========
    //
    // 协议: 客户端发 COMMAND|target_fd|payload base64 编码
    //       服务端转 COMMAND|from_fd|payload base64 编码
    // 媒体流走浏览器 P2P 直连，服务器只转发信令文本

    // ========== OFFER ==========
    // 发起方 SDP: OFFER|target_fd|sdp_base64
    this->on("OFFER", [](const AppMessage& msg,
                         std::shared_ptr<Session> session,
                         RoomManager& room_mgr) -> std::vector<Out> {
        std::vector<Out> results;
        if (msg.param_count() < 2) {
            return results;
        }
        int target_fd = 0;
        try {
            target_fd = std::stoi(msg.param(0));
        }
        catch (const std::exception& e) {
            std::cerr << "OFFER parse target_fd failed: " << e.what()
                    << " for param '" << msg.param(0) << "'" << std::endl;
            return results;
        }
        if (auto target = room_peer(room_mgr, session->room_, target_fd)) {
            results.push_back({target,
                AppParser::build("OFFER",
                    std::to_string(session->fd_), msg.param(1))});
        }
        return results;
    });

    // ========== ANSWER ==========
    // 接收方回复 SDP: ANSWER|target_fd|sdp_base64
    this->on("ANSWER", [](const AppMessage& msg,
                          std::shared_ptr<Session> session,
                          RoomManager& room_mgr) -> std::vector<Out> {
        std::vector<Out> results;
        if (msg.param_count() < 2) {
            return results;
        }
        int target_fd = 0;
        try {
            target_fd = std::stoi(msg.param(0));
        }
        catch (const std::exception& e) {
            std::cerr << "ANSWER parse target_fd failed: " << e.what()
                    << " for param '" << msg.param(0) << "'" << std::endl;
            return results;
        }
        if (auto target = room_peer(room_mgr, session->room_, target_fd)) {
            results.push_back({target,
                AppParser::build("ANSWER",
                    std::to_string(session->fd_), msg.param(1))});
        }
        return results;
    });

    // ========== ICE ==========
    this->on("ICE", [](const AppMessage& msg,
                       std::shared_ptr<Session> session,
                       RoomManager& room_mgr) -> std::vector<Out> {
        std::vector<Out> results;
        if (msg.param_count() < 2) {
            return results;
        }
        int target_fd = 0;
        try {
            target_fd = std::stoi(msg.param(0));
        }
        catch (const std::exception& e) {
            std::cerr << "ICE parse target_fd failed: " << e.what()
                    << " for param '" << msg.param(0) << "'" << std::endl;
            return results;
        }
        if (auto target = room_peer(room_mgr, session->room_, target_fd)) {
            results.push_back({target,
                AppParser::build("ICE",
                    std::to_string(session->fd_), msg.param(1))});
        }
        return results;
    });

    // ========== MEDIA ==========
    // 媒体状态通知
    // 客户端发: MEDIA|target_fd|kind|state
    // 服务端转: MEDIA|from_fd|kind|state
    this->on("MEDIA", [](const AppMessage& msg,
                         std::shared_ptr<Session> session,
                         RoomManager& room_mgr) -> std::vector<Out> {
        std::vector<Out> results;
        if (msg.param_count() < 3) {
            return results;
        }
        int target_fd = 0;
        try {
            target_fd = std::stoi(msg.param(0));
        }
        catch (const std::exception& e) {
            std::cerr << "MEDIA parse target_fd failed: " << e.what()
                    << " for param '" << msg.param(0) << "'" << std::endl;
            return results;
        }
        if (auto target = room_peer(room_mgr, session->room_, target_fd)) {
            results.push_back({target,
                AppParser::build("MEDIA",
                    std::to_string(session->fd_), msg.param(1), msg.param(2))});
        }
        return results;
    });
}
