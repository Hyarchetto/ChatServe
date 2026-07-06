// WebSocket 应用层消息路由
#include "ws/WebSocketRouter.h"
#include "ws/WebSocketAppParser.h"
#include "ws/WebSocketFrame.h"

WebSocketRouter::WebSocketRouter() {
    // ========== JOIN 处理器 ==========
    this->on("JOIN", [](const WebSocketAppMessage& msg,
                        const std::shared_ptr<Connection>& conn,
                        RoomManager& room_mgr) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;

        // 校验参数：JOIN|<room>|<nickname>
        if (msg.param_count() < 2 ||
            msg.param(0).empty() ||
            msg.param(1).empty()) {
            results.push_back({conn,
                WebSocketFrame::build(WebSocketOpcode::TEXT,
                    WebSocketAppParser::build("SYS", "ERR|JOIN 参数错误"))});
            return results;
        }

        conn->room_id_ = msg.param(0);
        conn->nickname_ = msg.param(1);

        auto room = room_mgr.get_or_create(conn->room_id_);
        room->add_num(conn);

        // 1. OK|<room>|<nick> 确认握手
        results.push_back({conn,
            WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("OK", conn->room_id_, conn->nickname_))});

        // 2. SYS|<nick> 加入房间 广播给其他人
        std::string sys = WebSocketFrame::build(WebSocketOpcode::TEXT,
            WebSocketAppParser::build("SYS", conn->nickname_ + " 加入房间"));
        for (auto& c : room->get_live_Connections()) {
            if (c->fd_ != conn->fd_) {
                results.push_back({c, sys});
            }
        }

        // 3. MEMBERS|<nick1>,<nick2>,... 发送给新加入者
        {
            std::string joined;
            auto live = room->get_live_Connections();
            for (size_t i = 0; i < live.size(); ++i) {
                if (i > 0) joined += ",";
                joined += live[i]->nickname_;
            }
            results.push_back({conn,
                WebSocketFrame::build(WebSocketOpcode::TEXT,
                    WebSocketAppParser::build("MEMBERS", joined))});
        }

        return results;
    });

    // ========== 默认处理器（裸文本 = 聊天消息） ==========
    this->on_default([](const WebSocketAppMessage& msg,
                         const std::shared_ptr<Connection>& conn,
                         RoomManager& room_mgr) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;
        if (conn->room_id_.empty()) return results;

        auto room = room_mgr.get_or_create(conn->room_id_);
        std::string wire = WebSocketFrame::build(WebSocketOpcode::TEXT,
            WebSocketAppParser::build("MSG", conn->nickname_, msg.raw_));

        for (auto& c : room->get_live_Connections()) {
            if (c->fd_ != conn->fd_) {
                results.push_back({c, wire});
            }
        }
        return results;
    });
}

void WebSocketRouter::on(const std::string& command, Handler handler) {
    this->handlers_[command] = std::move(handler);
}

void WebSocketRouter::on_default(Handler handler) {
    this->default_handler_ = std::move(handler);
}

bool WebSocketRouter::route(const WebSocketAppMessage& msg,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& room_mgr,
                             std::vector<WebSocketTargetedMessage>& out) const {
    out.clear();

    if (msg.is_command()) {
        auto it = this->handlers_.find(msg.command_);
        if (it != this->handlers_.end()) {
            out = it->second(msg, conn, room_mgr);
            return true;
        }
    }

    if (this->default_handler_) {
        out = this->default_handler_(msg, conn, room_mgr);
        return true;
    }

    return false;
}
