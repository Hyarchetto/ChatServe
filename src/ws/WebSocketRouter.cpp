// WebSocket 应用层消息路由
#include "ws/WebSocketRouter.h"
#include "ws/WebSocketAppParser.h"
#include "ws/WebSocketFrame.h"
#include "filetransfer/FileManager.h"


WebSocketRouter::WebSocketRouter() {
    // ========== JOIN 处理器 ==========
    this->on("JOIN", [](const WebSocketAppMessage& msg,
                        const std::shared_ptr<Connection>& conn,
                        RoomManager& room_mgr,
                        FileManager& /*file_mgr*/) -> std::vector<WebSocketTargetedMessage> {
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

        // ---------- 发给新加入者的确认 ----------
        // OK|<room>|<nick> 告诉前端收到 JOIN 了
        results.push_back({conn,
            WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("OK", conn->room_id_, conn->nickname_))});

        // ---------- SYS 消息给其他人 ----------
        // 纯显示用，让其他人在聊天框看到 "xxx 加入房间"
        // 不参与成员列表的维护，成员列表只认 MEMBERS
        std::string sys = WebSocketFrame::build(WebSocketOpcode::TEXT,
            WebSocketAppParser::build("SYS", conn->nickname_ + " 加入房间"));
        for (auto& c : room->get_live_connections()) {
            if (c->fd_ != conn->fd_) {
                results.push_back({c, sys});
            }
        }

        // ---------- MEMBERS 广播给所有人 ----------
        // 成员列表的唯一权威来源
        // 每次都全量推送，不搞增量增删，彻底避免同名用户导致列表不同步
        // 新加入者拿到房间全部成员，老成员也拿到更新后的完整列表
        {
            std::string joined;
            auto live = room->get_live_connections();
            for (size_t i = 0; i < live.size(); ++i) {
                if (i > 0) joined += ",";
                joined += live[i]->nickname_;
            }
            std::string members_frame = WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("MEMBERS", joined));
            for (auto& c : live) {
                results.push_back({c, members_frame});
            }
        }

        return results;
    });

    // ========== 默认处理器（裸文本 = 聊天消息） ==========
    this->on_default([](const WebSocketAppMessage& msg,
                         const std::shared_ptr<Connection>& conn,
                         RoomManager& room_mgr,
                         FileManager& /*file_mgr*/) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;
        if (conn->room_id_.empty()) return results;

        auto room = room_mgr.get_or_create(conn->room_id_);
        std::string wire = WebSocketFrame::build(WebSocketOpcode::TEXT,
            WebSocketAppParser::build("MSG", conn->nickname_, msg.raw_));

        for (auto& c : room->get_live_connections()) {
            if (c->fd_ != conn->fd_) {
                results.push_back({c, wire});
            }
        }
        return results;
    });

    // ========== UPLOAD 处理器 ==========
    this->on("UPLOAD", [](const WebSocketAppMessage& msg,
                           const std::shared_ptr<Connection>& conn,
                           RoomManager& /*room_mgr*/,
                           FileManager& file_mgr) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;
        if (msg.param_count() < 3) return results;

        std::string filename = msg.param(0);
        size_t filesize = 0;
        try { filesize = std::stoul(msg.param(1)); } catch (...) { return results; }
        std::string room_id = msg.param(2);

        std::string file_id = file_mgr.init_upload(
            filename, filesize, room_id, conn->nickname_, conn->fd_);
        if (file_id.empty()) return results;

        std::string resp = WebSocketFrame::build(WebSocketOpcode::TEXT,
            WebSocketAppParser::build("UPOK", file_id));
        results.push_back({conn, std::move(resp)});
        return results;
    });

    // ========== UPDONE 处理器（客户端主动通知上传完成） ==========
    this->on("UPDONE", [](const WebSocketAppMessage& msg,
                           const std::shared_ptr<Connection>& conn,
                           RoomManager& room_mgr,
                           FileManager& file_mgr) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;
        if (msg.param_count() < 1) return results;

        // 1. 尝试 finalize（如果 write_chunk 已自动完成，session 已不存在，返回空）
        FileMeta meta = file_mgr.finalize(conn->fd_);
        if (meta.file_id_.empty()) {
            // 可能已经自动完成 → 仍然回复 DONE 表示确认
            results.push_back({conn,
                WebSocketFrame::build(WebSocketOpcode::TEXT,
                    WebSocketAppParser::build("DONE", ""))});
            return results;
        }

        // 2. 构造 FILE 通知，广播给房间其他人
        std::string notify = WebSocketFrame::build(WebSocketOpcode::TEXT,
            WebSocketAppParser::build("FILE",
                meta.file_id_,
                meta.filename_,
                std::to_string(meta.filesize_),
                meta.sender_nickname_));

        auto room = room_mgr.get_or_create(meta.room_id_);
        for (auto& c : room->get_live_connections()) {
            if (c->fd_ != conn->fd_) {
                results.push_back({c, notify});
            }
        }

        // 3. 回复发送者 DONE 确认
        results.push_back({conn,
            WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("DONE", meta.file_id_))});

        return results;
    });

    // ========== UPCANCEL 处理器（取消上传） ==========
    this->on("UPCANCEL", [](const WebSocketAppMessage& /*msg*/,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& /*room_mgr*/,
                             FileManager& file_mgr) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;
        file_mgr.cancel_upload(conn->fd_);
        results.push_back({conn,
            WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("DONE", "cancelled"))});
        return results;
    });

    // ========== DOWNLOAD 处理器（大文件分块传输） ==========
    this->on("DOWNLOAD", [](const WebSocketAppMessage& msg,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& /*room_mgr*/,
                             FileManager& file_mgr) -> std::vector<WebSocketTargetedMessage> {
        std::vector<WebSocketTargetedMessage> results;
        if (msg.param_count() < 1) return results;

        std::string file_id = msg.param(0);
        FileMeta meta = file_mgr.get_meta(file_id);
        if (meta.file_id_.empty() || !meta.completed_) {
            results.push_back({conn,
                WebSocketFrame::build(WebSocketOpcode::TEXT,
                    WebSocketAppParser::build("SYS", "ERR|文件不存在或未完成"))});
            return results;
        }

        // 1. 先发文件元信息：DWINFO|file_id|filename|filesize
        results.push_back({conn,
            WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("DWINFO",
                    meta.file_id_,
                    meta.filename_,
                    std::to_string(meta.filesize_)))});

        // 2. 从内存读取文件并分块发送
        static constexpr size_t CHUNK_SIZE = 4 * 1024 * 1024; // 4MB 每块

        if (meta.data_.empty()) {
            results.push_back({conn,
                WebSocketFrame::build(WebSocketOpcode::TEXT,
                    WebSocketAppParser::build("SYS", "ERR|文件读取失败"))});
            return results;
        }

        // 计算总块数
        size_t total_chunks = (meta.filesize_ + CHUNK_SIZE - 1) / CHUNK_SIZE;
        if (total_chunks > 1) {
            // 多块 → 先发 DWCHUNK|file_id|total_chunks
            results.push_back({conn,
                WebSocketFrame::build(WebSocketOpcode::TEXT,
                    WebSocketAppParser::build("DWCHUNK",
                        meta.file_id_,
                        std::to_string(total_chunks)))});
        }

        // 从内存缓冲区逐块发送 BINARY 帧
        size_t offset = 0;
        while (offset < meta.data_.size()) {
            size_t bytes_this_chunk = std::min(CHUNK_SIZE, meta.data_.size() - offset);
            results.push_back({conn,
                WebSocketFrame::build(WebSocketOpcode::BINARY,
                    meta.data_.substr(offset, bytes_this_chunk))});
            offset += bytes_this_chunk;
        }

        // 3. 完成通知：DWNDONE|file_id
        results.push_back({conn,
            WebSocketFrame::build(WebSocketOpcode::TEXT,
                WebSocketAppParser::build("DWNDONE", meta.file_id_))});

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
                             FileManager& file_mgr,
                             std::vector<WebSocketTargetedMessage>& out) const {
    out.clear();

    if (msg.is_command()) {
        auto it = this->handlers_.find(msg.command_);
        if (it != this->handlers_.end()) {
            out = it->second(msg, conn, room_mgr, file_mgr);
            return true;
        }
    }

    if (this->default_handler_) {
        out = this->default_handler_(msg, conn, room_mgr, file_mgr);
        return true;
    }

    return false;
}
