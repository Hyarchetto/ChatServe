// 应用层命令路由 — 表机制与共享助手
#include <string>
#include <vector>

#include "app/AppRouter.h"
#include "app/AppParser.h"
#include "ctrl/Session.h"

// 初始化不同组件的协议
AppRouter::AppRouter(RoomManager& room_mgr) : room_mgr_(room_mgr) {
    this->register_chat();              // 聊天协议
    this->register_signalling();        // WebRTC协议
    this->register_transfer();          // 文件传输协议
}

// 分发命令，返回待发帧
std::vector<CtrlDown> AppRouter::handle(std::shared_ptr<Session> sess, const AppMessage& msg) {
    auto it = this->handlers_.find(msg.command_);
    if (it == this->handlers_.end()) {
        return {};
    }
    return it->second(std::move(sess), msg);
}

// 断开清理 只做房间离开与传输取消广播
std::vector<CtrlDown> AppRouter::cleanup(std::shared_ptr<Session> sess) {
    std::vector<CtrlDown> results;
    std::string room_id = sess->room_;
    // 未加入房间则房间查不到 传输清理与离开房间都自然空转

    // 1. 清理传输，通知受影响的下载方
    if (TransferManager* tm = find_transfer_mgr(room_id)) {
        auto cancel_info = tm->cancel_by_session(sess);
        for (auto& sc : cancel_info.cancelled_) {
            std::string dwerr = build_dwerr_frame(sc.file_id_, kUploaderGone);
            if (auto oc = sc.orphaned_downloader_) {
                results.push_back({std::move(oc), std::move(dwerr)});
            }
        }
    }

    // 2. 离开房间
    auto remaining = this->room_mgr_.leave_room(room_id, sess);
    sess->room_.clear();
    if (remaining.empty()) {
        return results;
    }

    // 3. 构造房间通知 只更新 MEMBERS 离开事件由 LEAVE 带 fd 客户端自行解析昵称
    std::string members_text = build_members_frame(remaining);

    std::string leave_text = AppParser::build_frame("LEAVE", std::to_string(sess->fd_));
    for (auto& c : remaining) {
        results.push_back({c, leave_text});
        results.push_back({c, members_text});
    }
    return results;
}

void AppRouter::on(const std::string& command, Handler handler) {
    this->handlers_[command] = std::move(handler);
}

// 广播帧给 live 列表里除 except 外的所有 Session
void AppRouter::broadcast_except(const std::vector<std::shared_ptr<Session>>& live, Session* except,
                                 const std::string& text, std::vector<CtrlDown>& results) {
    for (auto& c : live) {
        if (c.get() != except) {
            results.push_back({c, text});
        }
    }
}

// 广播帧给一个房间里除 except 外的所有 Session
void AppRouter::broadcast_to_room(const std::string& room_id, Session* except,
                                  const std::string& text, std::vector<CtrlDown>& results) {
    auto room = this->room_mgr_.find_room(room_id);
    if (!room) {
        return;
    }
    broadcast_except(room->get_connections(), except, text, results);
}

// 房间内按 fd 找目标 Session 房间查不到即无对象可发 只查不建
std::shared_ptr<Session> AppRouter::find_peer(const std::string& room_id, int target_fd) {
    auto room = this->room_mgr_.find_room(room_id);
    if (!room) {
        return nullptr;
    }
    for (auto& c : room->get_connections()) {
        if (c->fd_ == target_fd) {
            return c;
        }
    }
    return nullptr;
}

// 房间的文件传输管理器，传输状态归房间
TransferManager* AppRouter::find_transfer_mgr(const std::string& room_id) {
    auto room = this->room_mgr_.find_room(room_id);
    if (!room) {
        return nullptr;
    }
    return &room->transfer_mgr();
}

// MEMBERS 列表文本
std::string AppRouter::build_members_frame(const std::vector<std::shared_ptr<Session>>& live) {
    std::string joined;
    for (size_t i = 0; i < live.size(); ++i) {
        if (i > 0) {
            joined += ",";
        }
        joined += std::to_string(live[i]->fd_) + ":" + live[i]->nick_;
    }
    return AppParser::build_frame("MEMBERS", joined);
}

// 一条 DWERR 文本，寻址由调用点决定
std::string AppRouter::build_dwerr_frame(const std::string& file_id, const std::string& reason) {
    return AppParser::build_frame("DWERR", file_id, reason);
}

