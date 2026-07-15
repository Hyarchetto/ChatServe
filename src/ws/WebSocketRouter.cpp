// WebSocket 应用层消息路由 — 独立窗口文件传输
#include "ws/WebSocketRouter.h"
#include "ws/WebSocketAppParser.h"
#include "ws/WebSocketFrame.h"
#include "server/Connection.h"
#include "chatroom/Room.h"
#include "transfer/TransferManager.h"


WebSocketRouter::WebSocketRouter() {
    // ========== JOIN 处理器 ==========
    this->on("JOIN", [](const WebSocketAppMessage& msg,
                        const std::shared_ptr<Connection>& conn,
                        RoomManager& room_mgr,
                        TransferManager& /*transfer_mgr*/) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;

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

        // 确认，带上服务端分配的 id -- fd
        results.push_back({conn,
            WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("OK", conn->room_id_, conn->nickname_,
                    std::to_string(conn->fd_)))});

        // SYS 给其他人
        std::string sys = WebSocketFrame::build(WebSocketOpcode::TEXT,
            WebSocketAppParser::build("SYS", conn->nickname_ + " 加入房间"));
        for (auto& c : room->get_live_connections()) {
            if (c->fd_ != conn->fd_) {
                results.push_back({c, sys});
            }
        }

        // MEMBERS 全量广播
        {
            std::string joined;
            auto live = room->get_live_connections();
            for (size_t i = 0; i < live.size(); ++i) {
                if (i > 0) joined += ",";
                joined += std::to_string(live[i]->fd_) + ":" + live[i]->nickname_;
            }
            std::string members_frame = WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("MEMBERS", joined));
            for (auto& c : live) {
                results.push_back({c, members_frame});
            }
        }

        return results;
    });

    // ========== 默认处理器，裸文本作为聊天消息 ==========
    this->on_default([](const WebSocketAppMessage& msg,
                         const std::shared_ptr<Connection>& conn,
                         RoomManager& room_mgr,
                         TransferManager& /*transfer_mgr*/) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;
        if (conn->room_id_.empty()) return results;

        auto room = room_mgr.get_or_create(conn->room_id_);
        std::string wire = WebSocketFrame::build(WebSocketOpcode::TEXT,
            WebSocketAppParser::build("MSG",
                std::to_string(conn->fd_), conn->nickname_, msg.raw_));

        for (auto& c : room->get_live_connections()) {
            if (c->fd_ != conn->fd_) {
                results.push_back({c, wire});
            }
        }
        return results;
    });

    // ========== UPLOAD 处理器 ==========
    // 仅注册文件元数据，不上传文件内容
    this->on("UPLOAD", [](const WebSocketAppMessage& msg,
                           const std::shared_ptr<Connection>& conn,
                           RoomManager& room_mgr,
                           TransferManager& transfer_mgr) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;
        if (msg.param_count() < 3) return results;

        std::string filename = msg.param(0);
        size_t filesize = 0;
        try { filesize = std::stoul(msg.param(1)); } catch (...) { return results; }
        std::string room_id = msg.param(2);

        std::string file_id = transfer_mgr.register_file(
            filename, filesize, room_id, conn->nickname_, conn->fd_);
        if (file_id.empty()) return results;

        // 回复 UPOK 给上传方
        results.push_back({conn,
            WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("UPOK", file_id))});

        // 广播 FILE 通知给房间其他人，末尾带上上传方 fd 作为唯一标识
        std::string notify = WebSocketFrame::build(WebSocketOpcode::TEXT,
            WebSocketAppParser::build("FILE",
                {file_id, filename, std::to_string(filesize), conn->nickname_,
                 std::to_string(conn->fd_)}));

        auto room = room_mgr.get_or_create(room_id);
        for (auto& c : room->get_live_connections()) {
            if (c->fd_ != conn->fd_) {
                results.push_back({c, notify});
            }
        }

        return results;
    });

    // ========== UPCANCEL 处理器 ==========
    this->on("UPCANCEL", [](const WebSocketAppMessage& /*msg*/,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& /*room_mgr*/,
                             TransferManager& transfer_mgr) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;
        transfer_mgr.unregister_by_uploader(conn->fd_);
        results.push_back({conn,
            WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("DONE", "cancelled"))});
        return results;
    });

    // ========== DOWNLOAD 处理器 ==========
    // 启动独立窗口传输
    this->on("DOWNLOAD", [](const WebSocketAppMessage& msg,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& room_mgr,
                             TransferManager& transfer_mgr) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;
        if (msg.param_count() < 1) return results;

        std::string file_id = msg.param(0);
        auto reg = transfer_mgr.get_registration(file_id);
        if (reg.file_id_.empty()) {
            results.push_back({conn,
                WebSocketFrame::build(WebSocketOpcode::TEXT,
                    WebSocketAppParser::build("SYS", "ERR|文件不存在"))});
            return results;
        }

        // 启动传输，获取初始窗口请求
        uint64_t session_id = 0;
        auto init_reqs = transfer_mgr.start_transfer(file_id, conn->fd_, session_id);
        if (init_reqs.empty()) {
            results.push_back({conn,
                WebSocketFrame::build(WebSocketOpcode::TEXT,
                    WebSocketAppParser::build("SYS", "ERR|无法启动传输 上传方可能已离线"))});
            return results;
        }

        // DWSTART 给下载方
        results.push_back({conn,
            WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("DWSTART",
                    reg.file_id_, reg.filename_, std::to_string(reg.filesize_)))});

        // 在房间中找到上传方并发 DWREQ，带上 session_id
        auto room = room_mgr.get_or_create(reg.room_id_);
        for (auto& c : room->get_live_connections()) {
            if (c->fd_ == reg.uploader_fd_) {
                for (auto& req : init_reqs) {
                    results.push_back({c,
                        WebSocketFrame::build(WebSocketOpcode::TEXT,
                            WebSocketAppParser::build("DWREQ",
                                std::to_string(req.session_id_),
                                req.file_id_,
                                std::to_string(req.offset_),
                                std::to_string(req.size_)))});
                }
                break;
            }
        }

        return results;
    });

    // ========== DWACK 处理器 ==========
    // 下载方确认收到一个分块，触发下一个 DWREQ
    // 协议: DWACK|<session_id>|<offset>
    this->on("DWACK", [](const WebSocketAppMessage& msg,
                          const std::shared_ptr<Connection>& conn,
                          RoomManager& room_mgr,
                          TransferManager& transfer_mgr) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;
        if (msg.param_count() < 2) return results;

        uint64_t session_id = 0;
        size_t offset = 0;
        try {
            session_id = std::stoull(msg.param(0));
            offset = std::stoul(msg.param(1));
        } catch (...) { return results; }

        auto ar = transfer_mgr.handle_ack(session_id, offset);
        if (!ar.valid_) return results;

        // 该下载方完成
        if (ar.downloader_done_) {
            results.push_back({conn,
                WebSocketFrame::build(WebSocketOpcode::TEXT,
                    WebSocketAppParser::build("DWNDONE", ar.file_id_))});
        }

        // 发送下一个 DWREQ 给上传方
        if (ar.has_next_req_) {
            // 从文件注册获取房间信息以定位上传方 Connection
            auto reg = transfer_mgr.get_registration(ar.file_id_);
            bool found = false;
            if (!reg.file_id_.empty()) {
                auto room = room_mgr.get_or_create(reg.room_id_);
                for (auto& c : room->get_live_connections()) {
                    if (c->fd_ == ar.next_req_uploader_fd_) {
                        auto dwreq = WebSocketFrame::build(WebSocketOpcode::TEXT,
                            WebSocketAppParser::build("DWREQ",
                                std::to_string(ar.next_req_session_id_),
                                ar.file_id_,
                                std::to_string(ar.next_req_offset_),
                                std::to_string(ar.next_req_size_)));
                        results.push_back({c, std::move(dwreq)});
                        found = true;
                        break;
                    }
                }
            }
            // 上传方已不在房间，通知下载方
            if (!found) {
                results.push_back({conn,
                    WebSocketFrame::build(WebSocketOpcode::TEXT,
                        WebSocketAppParser::build("DWERR",
                            ar.file_id_, "上传方已离开，下载失败"))});
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
                             TransferManager& transfer_mgr,
                             std::vector<WebSocketTargetedMessage>& out) const {

    if (msg.is_command()) {
        auto it = this->handlers_.find(msg.command_);
        if (it != this->handlers_.end()) {
            out = it->second(msg, conn, room_mgr, transfer_mgr);
            return true;
        }
    }

    if (this->default_handler_) {
        out = this->default_handler_(msg, conn, room_mgr, transfer_mgr);
        return true;
    }

    return false;
}
