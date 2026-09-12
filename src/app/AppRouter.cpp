// 应用层命令路由 — 表机制与共享助手
#include <string>
#include <vector>

#include "app/AppRouter.h"
#include "app/AppParser.h"
#include "ctrl/Session.h"

// 广播帧给 live 列表里除 except 外的所有 Session
void AppRouter::broadcast_except(
    const std::vector<std::shared_ptr<Session>>& live, Session* except,
    std::vector<Out>& results, const std::string& text) {
    for (auto& c : live) {
        if (c.get() != except) {
            results.push_back({c, text});
        }
    }
}

// 向某房间广播一帧给除 except 外的成员 结果追加进 results
void AppRouter::broadcast_to_room(RoomManager& room_mgr,
                                  const std::string& room_id, Session* except,
                                  const std::string& text,
                                  std::vector<Out>& results) {
    auto room = room_mgr.get_or_create(room_id);
    broadcast_except(room->get_live_connections(), except, results, text);
}

// 房间的文件传输管理器 传输状态归房间
TransferManager& AppRouter::transfer_mgr_of(RoomManager& room_mgr,
                                            const std::string& room_id) {
    return room_mgr.get_or_create(room_id)->transfer_mgr();
}

// MEMBERS 列表文本 fd:nick 逗号分隔 客户端昵称表据此建立
std::string AppRouter::members_frame(
    const std::vector<std::shared_ptr<Session>>& live) {
    std::string joined;
    for (size_t i = 0; i < live.size(); ++i) {
        if (i > 0) {
            joined += ",";
        }
        joined += std::to_string(live[i]->fd_) + ":" + live[i]->nick_;
    }
    return AppParser::build("MEMBERS", joined);
}

// 一条 DWERR 文本 寻址由调用点决定
std::string AppRouter::dwerr_frame(const std::string& file_id,
                                   const std::string& reason) {
    return AppParser::build("DWERR", file_id, reason);
}

AppRouter::AppRouter() {
    this->register_chat();
    this->register_signalling();
    this->register_transfer();
}

void AppRouter::on(const std::string& command, Handler handler) {
    this->handlers_[command] = std::move(handler);
}

void AppRouter::on_default(Handler handler) {
    this->default_handler_ = std::move(handler);
}

std::vector<AppRouter::Out> AppRouter::handle(
    const AppMessage& msg, std::shared_ptr<Session> session,
    RoomManager& room_mgr) {
    if (msg.is_command()) {
        auto it = this->handlers_.find(msg.command_);
        if (it != this->handlers_.end()) {
            return it->second(msg, session, room_mgr);
        }
    }
    if (this->default_handler_) {
        return this->default_handler_(msg, session, room_mgr);
    }
    return {};
}

// 断开清理 与旧实现语义一致 只做房间离开与传输取消广播
// 跨域故留在这里 前半清传输后半离房间
std::vector<AppRouter::Out> AppRouter::cleanup(
    std::shared_ptr<Session> session, RoomManager& room_mgr) {
    std::vector<Out> results;
    // 1. 清理传输，通知受影响的下载方
    std::string room_id = session->room_;
    if (!room_id.empty()) {
        TransferManager& tm = transfer_mgr_of(room_mgr, room_id);
        auto cancel_info = tm.cancel_by_conn(session);
        for (auto& sc : cancel_info.cancelled_) {
            std::string dwerr = dwerr_frame(
                sc.file_id_, "上传方已离开，下载失败");
            if (auto oc = sc.orphaned_downloader_) {
                results.push_back({std::move(oc), std::move(dwerr)});
            }
        }
    }

    // 2. 没加入房间则只送 DWERR 后返回
    if (room_id.empty()) {
        return results;
    }

    // 3. 离开房间
    auto remaining = room_mgr.leave_room(room_id, session);
    session->room_.clear();
    if (remaining.empty()) {
        return results;
    }

    // 4. 构造房间通知 只更新 MEMBERS 离开事件由 LEAVE 带 fd 客户端自行解析昵称
    std::string members_text = members_frame(remaining);

    std::string leave_text = AppParser::build("LEAVE",
        std::to_string(session->fd_));
    for (auto& c : remaining) {
        if (c->alive_) {
            results.push_back({c, leave_text});
            results.push_back({c, members_text});
        }
    }
    return results;
}
