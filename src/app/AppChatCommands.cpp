// 应用层命令路由 — 聊天与房间域
#include <string>
#include <vector>

#include "app/AppRouter.h"
#include "app/AppParser.h"
#include "ctrl/Session.h"

void AppRouter::register_chat() {
    // ========== JOIN 处理器 ==========
    this->on("JOIN", [this](std::shared_ptr<Session> sess,
                            const AppMessage& msg) -> std::vector<CtrlDown> {
        std::vector<CtrlDown> results;
        // 协议错误，回复错误响应
        if (msg.param_count() < 2 || msg.param(0).empty() || msg.param(1).empty()) {
            results.push_back({sess, AppParser::build_frame("SYS", "ERR|JOIN 参数错误")});
            return results;
        }
        // 协议重复，丢包
        if (!sess->room_.empty()) {
            return results;
        }
        // 获取房间
        auto room = this->room_mgr_.get_or_create(msg.param(0));
        if (!room->add_num(sess)) {
            results.push_back({sess,
                AppParser::build_frame("SYS","ERR|房间已满（上限 " + std::to_string(Room::kMaxMembers) + " 人）")});
            return results;
        }
        // 设置session参数
        sess->room_ = msg.param(0);
        sess->nick_ = msg.param(1);
        // 返回ACK包
        results.push_back({sess, AppParser::build_frame("OK", sess->room_, std::to_string(sess->fd_))});
        // 获取房间成员列表
        auto members = room->get_connections();
        // 广播给房间内所有成员，包括自己
        std::string members_text = build_members_frame(members);
        broadcast_except(members, nullptr, members_text, results);
        // 加入通知只带 fd 昵称由客户端从本地映射解析
        std::string join_text = AppParser::build_frame("JOIN",std::to_string(sess->fd_));
        broadcast_except(members, sess.get(), join_text, results);

        return results;
    });

    // ========== 聊天消息 ==========
    // 收到 MSG|内容 广播给房间内除自己外的所有人
    this->on("MSG", [this](std::shared_ptr<Session> sess,
                           const AppMessage& msg) -> std::vector<CtrlDown> {
        std::vector<CtrlDown> results;
        // 未加入房间则房间查不到 广播自然空转
        std::string room_id = sess->room_;
        // 构建文本帧 只带发送者 fd 昵称由客户端从本地映射解析
        std::string wire = AppParser::build_frame("MSG", std::to_string(sess->fd_),
                                                  msg.rest_);
        this->broadcast_to_room(room_id, sess.get(), wire, results);
        
        return results;
    });
}
