// 应用层命令路由 — 聊天与房间域
#include <string>
#include <vector>

#include "app/AppRouter.h"
#include "app/AppParser.h"
#include "ctrl/Session.h"

void AppRouter::register_chat() {
    // ========== JOIN 处理器 ==========
    this->on("JOIN", [](const AppMessage& msg,
                        std::shared_ptr<Session> session,
                        RoomManager& room_mgr) -> std::vector<Out> {
        std::vector<Out> results;

        if (msg.param_count() < 2 ||
            msg.param(0).empty() ||
            msg.param(1).empty()) {
            results.push_back({session,
                AppParser::build("SYS", "ERR|JOIN 参数错误")});
            return results;
        }

        // 房间人数上限检查 最多 9 人
        auto room = room_mgr.get_or_create(msg.param(0));
        {
            auto live = room->get_live_connections();
            if (live.size() >= 9) {
                results.push_back({session,
                    AppParser::build("SYS", "ERR|房间已满（上限 9 人）")});
                return results;
            }
        }

        session->room_ = msg.param(0);
        session->nick_ = msg.param(1);
        room->add_num(session);

        // 确认 服务端分配的 id 就是 fd 客户端只认 fd
        results.push_back({session,
            AppParser::build("OK", session->room_, std::to_string(session->fd_))});

        // MEMBERS 带 fd:nick 映射 是客户端本地昵称表的唯一来源
        // 先发 MEMBERS 再发 JOIN 让其他成员能查到新加入者的昵称
        auto live = room->get_live_connections();
        {
            std::string members_text = members_frame(live);
            // 发给所有成员含新加入者 新加入者需要成员列表来建立 P2P 连接
            for (auto& c : live) {
                results.push_back({c, members_text});
            }
        }

        // 加入通知只带 fd 昵称由客户端从本地映射解析
        std::string join_text = AppParser::build("JOIN",
            std::to_string(session->fd_));
        broadcast_except(live, session.get(), results, join_text);

        return results;
    });

    // ========== 聊天消息 ==========
    // MSG|fd|内容
    auto broadcast_chat = [](const std::string& content,
                             std::shared_ptr<Session> session,
                             RoomManager& room_mgr) -> std::vector<Out> {
        std::vector<Out> results;
        std::string room_id = session->room_;
        // 未加入房间不广播
        if (room_id.empty()) {
            return results;
        }
        // 构建文本帧 只带发送者 fd 昵称由客户端从本地映射解析
        std::string wire = AppParser::build("MSG",
            std::to_string(session->fd_), content);
        broadcast_to_room(room_mgr, room_id, session.get(), wire, results);
        return results;
    };

    this->on("MSG", [broadcast_chat](const AppMessage& msg,
                                     std::shared_ptr<Session> session,
                                     RoomManager& room_mgr) -> std::vector<Out> {
        return broadcast_chat(msg.raw_.substr(msg.command_.size() + 1),
                              session, room_mgr);
    });

    this->on_default([broadcast_chat](const AppMessage& msg,
                                      std::shared_ptr<Session> session,
                                      RoomManager& room_mgr) -> std::vector<Out> {
        return broadcast_chat(msg.raw_, session, room_mgr);
    });
}
