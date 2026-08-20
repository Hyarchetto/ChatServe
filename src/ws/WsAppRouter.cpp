// WebSocket 应用层消息路由 — 独立窗口文件传输
#include <iostream>
#include "ws/WsAppRouter.h"
#include "ws/WsAppParser.h"
#include "ws/WsFrame.h"
#include "server/Connection.h"
#include "chatroom/Room.h"
#include "transfer/TransferManager.h"

// 在房间连接列表里按 fd 找目标连接，找不到返回 nullptr
static std::shared_ptr<Connection> find_connection(
    const std::vector<std::shared_ptr<Connection>>& live, int fd) {
    for (auto& c : live) {
        if (c->fd_ == fd) return c;
    }
    return nullptr;
}

// 广播帧给 live 列表里除 except_fd 外的所有连接
static void broadcast_except(
    const std::vector<std::shared_ptr<Connection>>& live, int except_fd,
    std::vector<WsTargetedMessage>& results, const std::string& frame) {
    for (auto& c : live) {
        if (c->fd_ != except_fd) {
            results.push_back({c, frame});
        }
    }
}

WsAppRouter::WsAppRouter() {
    // ========== JOIN 处理器 ==========
    this->on("JOIN", [](const WsAppMessage& msg,
                        const std::shared_ptr<Connection>& conn,
                        RoomManager& room_mgr,
                        TransferManager& /*transfer_mgr*/) -> std::vector<WsTargetedMessage> {
        std::vector<WsTargetedMessage> results;

        if (msg.param_count() < 2 ||
            msg.param(0).empty() ||
            msg.param(1).empty()) {
            results.push_back({conn,
                WsFrame::build(WsOpcode::TEXT,
                    WsAppParser::build("SYS", "ERR|JOIN 参数错误"))});
            return results;
        }

        // 房间人数上限检查 最多 9 人
        auto room = room_mgr.get_or_create(msg.param(0));
        {
            auto live = room->get_live_connections();
            if (live.size() >= 9) {
                results.push_back({conn,
                    WsFrame::build(WsOpcode::TEXT,
                        WsAppParser::build("SYS", "ERR|房间已满（上限 9 人）"))});
                return results;
            }
        }

        conn->set_identity(msg.param(0), msg.param(1));
        room->add_num(conn);

        // 确认，带上服务端分配的 id -- fd
        results.push_back({conn,
            WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("OK", conn->get_room_id(), conn->get_nickname(),
                    std::to_string(conn->fd_)))});

        // SYS + MEMBERS 共用一次 get_live_connections
        auto live = room->get_live_connections();
        std::string sys = WsFrame::build(WsOpcode::TEXT,
            WsAppParser::build("SYS", conn->get_nickname() + " 加入房间"));
        broadcast_except(live, conn->fd_, results, sys);

        {
            std::string joined;
            for (size_t i = 0; i < live.size(); ++i) {
                if (i > 0) joined += ",";
                joined += std::to_string(live[i]->fd_) + ":" + live[i]->get_nickname();
            }
            std::string members_frame = WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("MEMBERS", joined));
            broadcast_except(live, conn->fd_, results, members_frame);
        }

        return results;
    });

    // ========== 聊天消息 ==========
    // MSG|fd|昵称|内容
    auto broadcast_chat = [](const std::string& content,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& room_mgr) -> std::vector<WsTargetedMessage> {
        // 获取连接的房间号
        std::vector<WsTargetedMessage> results;
        if (conn->get_room_id().empty()) return results;
        // 获取房间
        auto room = room_mgr.get_or_create(conn->get_room_id());
        // 构建文本帧
        std::string wire = WsFrame::build(WsOpcode::TEXT,
            WsAppParser::build("MSG",
                std::to_string(conn->fd_), conn->get_nickname(), content));
        
        broadcast_except(room->get_live_connections(), conn->fd_, results, wire);
        return results;
    };

    this->on("MSG", [broadcast_chat](const WsAppMessage& msg,
                                     const std::shared_ptr<Connection>& conn,
                                     RoomManager& room_mgr,
                                     TransferManager& /*transfer_mgr*/) -> std::vector<WsTargetedMessage> {
        return broadcast_chat(msg.raw_.substr(msg.command_.size() + 1), conn, room_mgr);
    });

    this->on_default([broadcast_chat](const WsAppMessage& msg,
                                      const std::shared_ptr<Connection>& conn,
                                      RoomManager& room_mgr,
                                      TransferManager& /*transfer_mgr*/) -> std::vector<WsTargetedMessage> {
        return broadcast_chat(msg.raw_, conn, room_mgr);
    });

    // ========== UPLOAD 处理器 ==========
    // 仅注册文件元数据，不上传文件内容
    this->on("UPLOAD", [](const WsAppMessage& msg,
                           const std::shared_ptr<Connection>& conn,
                           RoomManager& room_mgr,
                           TransferManager& transfer_mgr) -> std::vector<WsTargetedMessage> {
        std::vector<WsTargetedMessage> results;
        if (msg.param_count() < 2) return results;

        std::string filename = msg.param(0);
        size_t filesize = 0;
        try { 
            filesize = std::stoul(msg.param(1)); 
        } 
        catch (const std::exception& e) {
            std::cerr << "UPLOAD filesize parse failed: " << e.what() 
                    << " for param '" << msg.param(1) << "'" << std::endl;
            return results;
        }

        std::string file_id = transfer_mgr.register_file(
            filename, filesize, conn->get_room_id(), conn->fd_);
        if (file_id.empty()) return results;

        // 回复 UPOK 给上传方
        results.push_back({conn,
            WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("UPOK", file_id))});

        // 广播 FILE 通知给房间其他人，末尾带上上传方 fd 作为唯一标识
        std::string notify = WsFrame::build(WsOpcode::TEXT,
            WsAppParser::build("FILE",
                {file_id, filename, std::to_string(filesize), conn->get_nickname(),
                 std::to_string(conn->fd_)}));

        auto room = room_mgr.get_or_create(conn->get_room_id());
        broadcast_except(room->get_live_connections(), conn->fd_, results, notify);

        return results;
    });

    // ========== UPCANCEL 处理器 ==========
    // 上传方取消单个文件，通知被孤立的下载方
    this->on("UPCANCEL", [](const WsAppMessage& msg,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& room_mgr,
                             TransferManager& transfer_mgr) -> std::vector<WsTargetedMessage> {
        std::vector<WsTargetedMessage> results;
        if (msg.param_count() < 1) {
            return results;
        }
        std::string file_id = msg.param(0);

        // 校验归属：文件存在且属于当前上传方
        auto reg = transfer_mgr.get_registration(file_id);
        if (reg.file_id_.empty() || reg.uploader_fd_ != conn->fd_) {
            return results;
        }

        transfer_mgr.cancel_file(file_id);

        // 广播文件失效，房间内所有下载方卡片显示已失效，与退出房间一致
        std::string dwerr = WsFrame::build(WsOpcode::TEXT,
            WsAppParser::build("DWERR", file_id, "上传已取消"));
        auto room = room_mgr.get_or_create(reg.room_id_);
        broadcast_except(room->get_live_connections(), conn->fd_, results, dwerr);

        results.push_back({conn,
            WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("DONE", "cancelled"))});
        return results;
    });

    // ========== DOWNLOAD 处理器 ==========
    // 启动独立窗口传输
    this->on("DOWNLOAD", [](const WsAppMessage& msg,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& room_mgr,
                             TransferManager& transfer_mgr) -> std::vector<WsTargetedMessage> {
        std::vector<WsTargetedMessage> results;
        if (msg.param_count() < 1) return results;

        std::string file_id = msg.param(0);
        auto reg = transfer_mgr.get_registration(file_id);
        if (reg.file_id_.empty()) {
            results.push_back({conn,
                WsFrame::build(WsOpcode::TEXT,
                    WsAppParser::build("SYS", "ERR|文件不存在"))});
            return results;
        }

        // 断点续传偏移，普通下载为 0
        size_t start_offset = 0;
        if (msg.param_count() >= 2) {
            try {
                start_offset = std::stoull(msg.param(1));
            } 
            catch (const std::exception& e) {
                std::cerr << "DOWNLOAD start_offset parse failed, fallback to 0: " 
                        << e.what() << " for param '" << msg.param(1) << "'" << std::endl;
                // start_offset 保持为 0
            }
        }

        // 启动传输，获取初始窗口请求
        uint64_t session_id = 0;
        auto init_reqs = transfer_mgr.start_transfer(file_id, conn->fd_, start_offset, session_id);
        if (init_reqs.empty()) {
            results.push_back({conn,
                WsFrame::build(WsOpcode::TEXT,
                    WsAppParser::build("SYS", "ERR|无法启动传输 上传方可能已离线"))});
            return results;
        }

        // DWSTART 给下载方
        results.push_back({conn,
            WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("DWSTART",
                    reg.file_id_, reg.filename_, std::to_string(reg.filesize_)))});

        // 在房间中找到上传方并发 DWREQ，带上 session_id
        auto room = room_mgr.get_or_create(reg.room_id_);
        if (auto uploader = find_connection(room->get_live_connections(), reg.uploader_fd_)) {
            for (auto& req : init_reqs) {
                results.push_back({uploader,
                    WsFrame::build(WsOpcode::TEXT,
                        WsAppParser::build("DWREQ",
                            std::to_string(req.session_id_),
                            req.file_id_,
                            std::to_string(req.offset_),
                            std::to_string(req.size_)))});
            }
        }

        return results;
    });

    // ========== DWACK 处理器 ==========
    // 下载方确认收到一个分块，触发下一个 DWREQ
    // 协议: DWACK|<session_id>|<offset>
    this->on("DWACK", [](const WsAppMessage& msg,
                          const std::shared_ptr<Connection>& conn,
                          RoomManager& room_mgr,
                          TransferManager& transfer_mgr) -> std::vector<WsTargetedMessage> {
        std::vector<WsTargetedMessage> results;
        if (msg.param_count() < 2) {
            return results;
        }
        uint64_t session_id = 0;
        size_t offset = 0;
        try {
            session_id = std::stoull(msg.param(0));
            offset = std::stoul(msg.param(1));
        } 
        catch (const std::exception& e) {
            std::cerr << "DWACK parse failed: " << e.what() 
                      << " for params [" << msg.param(0) << ", " << msg.param(1) << "]" << std::endl;
            return results;
        }

        auto ar = transfer_mgr.handle_ack(session_id, offset);
        if (!ar.valid_) {
            return results;
        }
        // 该下载方完成，仅通知下载方，上传方文件仍保持可下载
        if (ar.downloader_done_) {
            results.push_back({conn,
                WsFrame::build(WsOpcode::TEXT,
                    WsAppParser::build("DWNDONE", ar.file_id_))});
        }

        // 发送下一个 DWREQ 给上传方
        if (ar.next_) {
            // 从文件注册获取房间信息以定位上传方 Connection
            auto reg = transfer_mgr.get_registration(ar.next_->file_id_);
            std::shared_ptr<Connection> uploader;
            if (!reg.file_id_.empty()) {
                uploader = find_connection(room_mgr.get_or_create(reg.room_id_)->get_live_connections(),
                                           ar.next_->uploader_fd_);
            }
            if (uploader) {
                results.push_back({uploader,
                    WsFrame::build(WsOpcode::TEXT,
                        WsAppParser::build("DWREQ",
                            std::to_string(ar.next_->session_id_),
                            ar.next_->file_id_,
                            std::to_string(ar.next_->offset_),
                            std::to_string(ar.next_->size_)))});
            }
            // 上传方已不在房间，通知下载方
            else {
                results.push_back({conn,
                    WsFrame::build(WsOpcode::TEXT,
                        WsAppParser::build("DWERR",
                            ar.next_->file_id_, "上传方已离开，下载失败"))});
            }
        }

        return results;
    });

    // ========== 下载方会话控制 ==========
    // DWNPAUSE 暂停下载，DWNCANCEL 取消下载，都只取消当前会话
    // 协议: DWNPAUSE|<file_id>  /  DWNCANCEL|<file_id>
    auto cancel_session = [](const WsAppMessage& msg,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& /*room_mgr*/,
                             TransferManager& transfer_mgr) -> std::vector<WsTargetedMessage> {
        std::vector<WsTargetedMessage> results;
        if (msg.param_count() < 1) {
            return results;
        }
        transfer_mgr.cancel_session(msg.param(0), conn->fd_);
        return results;
    };
    this->on("DWNPAUSE", cancel_session);
    this->on("DWNCANCEL", cancel_session);

    // ========== WebRTC 信令转发 ==========
    //
    // 协议: 客户端发 COMMAND|target_fd|payload base64 编码
    //       服务端转 COMMAND|from_fd|from_nick|payload base64 编码
    // 媒体流走浏览器 P2P 直连，服务器只转发信令文本

    // ========== OFFER ==========
    // 发起方 SDP: OFFER|target_fd|sdp_base64
    this->on("OFFER", [](const WsAppMessage& msg,
                         const std::shared_ptr<Connection>& conn,
                         RoomManager& room_mgr,
                         TransferManager& /*transfer_mgr*/) -> std::vector<WsTargetedMessage> {
        std::vector<WsTargetedMessage> results;
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
        auto room = room_mgr.get_or_create(conn->get_room_id());
        if (auto target = find_connection(room->get_live_connections(), target_fd)) {
            results.push_back({target, WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("OFFER",
                    std::to_string(conn->fd_), conn->get_nickname(), msg.param(1)))});
        }
        return results;
    });

    // ========== ANSWER ==========
    // 接收方回复 SDP: ANSWER|target_fd|sdp_base64
    this->on("ANSWER", [](const WsAppMessage& msg,
                          const std::shared_ptr<Connection>& conn,
                          RoomManager& room_mgr,
                          TransferManager& /*transfer_mgr*/) -> std::vector<WsTargetedMessage> {
        std::vector<WsTargetedMessage> results;
        // 
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
        auto room = room_mgr.get_or_create(conn->get_room_id());
        if (auto target = find_connection(room->get_live_connections(), target_fd)) {
            results.push_back({target, WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("ANSWER",
                    std::to_string(conn->fd_), msg.param(1)))});
        }
        return results;
    });

    // ========== ICE ==========
    this->on("ICE", [](const WsAppMessage& msg,
                       const std::shared_ptr<Connection>& conn,
                       RoomManager& room_mgr,
                       TransferManager& /*transfer_mgr*/) -> std::vector<WsTargetedMessage> {
        std::vector<WsTargetedMessage> results;
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
        auto room = room_mgr.get_or_create(conn->get_room_id());
        if (auto target = find_connection(room->get_live_connections(), target_fd)) {
            results.push_back({target, WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("ICE",
                    std::to_string(conn->fd_), msg.param(1)))});
        }
        return results;
    });

    // ========== MEDIA ==========
    // 媒体状态通知
    // 客户端发: MEDIA|target_fd|kind|state
    // 服务端转: MEDIA|from_fd|kind|state
    this->on("MEDIA", [](const WsAppMessage& msg,
                         const std::shared_ptr<Connection>& conn,
                         RoomManager& room_mgr,
                         TransferManager& /*transfer_mgr*/) -> std::vector<WsTargetedMessage> {
        std::vector<WsTargetedMessage> results;
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
        auto room = room_mgr.get_or_create(conn->get_room_id());
        if (auto target = find_connection(room->get_live_connections(), target_fd)) {
            results.push_back({target, WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("MEDIA",
                    std::to_string(conn->fd_), msg.param(1), msg.param(2)))});
        }
        return results;
    });
}

void WsAppRouter::on(const std::string& command, Handler handler) {
    this->handlers_[command] = std::move(handler);
}

void WsAppRouter::on_default(Handler handler) {
    this->default_handler_ = std::move(handler);
}

bool WsAppRouter::handle(const WsAppMessage& msg,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& room_mgr,
                             TransferManager& transfer_mgr,
                             std::vector<WsTargetedMessage>& out) const {

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
