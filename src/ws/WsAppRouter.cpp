// WebSocket 应用层消息路由 — 独立窗口文件传输
#include <iostream>
#include "ws/WsAppRouter.h"
#include "ws/WsAppParser.h"
#include "ws/WsFrame.h"
#include "conn/Connection.h"
#include "chatroom/Room.h"
#include "chatroom/TransferManager.h"

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

// 向某房间广播一帧给除 except_fd 外的成员 结果追加进 results
static void broadcast_to_room(RoomManager& room_mgr,
                              const std::string& room_id, int except_fd,
                              const std::string& frame,
                              std::vector<WsTargetedMessage>& results) {
    auto room = room_mgr.get_or_create(room_id);
    broadcast_except(room->get_live_connections(), except_fd, results, frame);
}

// 房间内按 fd 找目标连接 找不到返回 nullptr
static std::shared_ptr<Connection> room_peer(RoomManager& room_mgr,
                                             const std::string& room_id,
                                             int target_fd) {
    auto room = room_mgr.get_or_create(room_id);
    return find_connection(room->get_live_connections(), target_fd);
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

        // 确认 服务端分配的 id 就是 fd 客户端只认 fd
        results.push_back({conn,
            WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("OK", conn->get_room_id(),
                    std::to_string(conn->fd_)))});

        // MEMBERS 带 fd:nick 映射 是客户端本地昵称表的唯一来源
        // 先发 MEMBERS 再发 JOIN 让其他成员能查到新加入者的昵称
        auto live = room->get_live_connections();
        {
            std::string joined;
            for (size_t i = 0; i < live.size(); ++i) {
                if (i > 0) {
                    joined += ",";
                }
                joined += std::to_string(live[i]->fd_) + ":" + live[i]->get_nickname();
            }
            std::string members_frame = WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("MEMBERS", joined));
            // 发给所有成员含新加入者 新加入者需要成员列表来建立 P2P 连接
            for (auto& c : live) {
                results.push_back({c, members_frame});
            }
        }

        // 加入通知只带 fd 昵称由客户端从本地映射解析
        std::string join_frame = WsFrame::build(WsOpcode::TEXT,
            WsAppParser::build("JOIN", std::to_string(conn->fd_)));
        broadcast_except(live, conn->fd_, results, join_frame);

        return results;
    });

    // ========== 聊天消息 ==========
    // MSG|fd|内容
    auto broadcast_chat = [](const std::string& content,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& room_mgr) -> std::vector<WsTargetedMessage> {
        std::vector<WsTargetedMessage> results;
        std::string room_id = conn->get_room_id();
        // 未加入房间不广播
        if (room_id.empty()) return results;
        // 构建文本帧 只带发送者 fd 昵称由客户端从本地映射解析
        std::string wire = WsFrame::build(WsOpcode::TEXT,
            WsAppParser::build("MSG",
                std::to_string(conn->fd_), content));
        broadcast_to_room(room_mgr, room_id, conn->fd_, wire, results);
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
        // 未加入房间的防御性检查 与 MSG 对齐 防止文件注册到空房间
        std::string room_id = conn->get_room_id();
        if (room_id.empty()) return results;

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
            filename, filesize, conn);
        if (file_id.empty()) {
            return results;
        }
        // 回复 UPOK 给上传方
        results.push_back({conn,
            WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("UPOK", file_id))});

        // 广播 FILE 通知给房间其他人 末尾带上上传方 fd 作为唯一标识
        std::string notify = WsFrame::build(WsOpcode::TEXT,
            WsAppParser::build("FILE",
                {file_id, filename, std::to_string(filesize), std::to_string(conn->fd_)}));

        broadcast_to_room(room_mgr, room_id, conn->fd_, notify, results);

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
        if (reg.file_id_.empty() || reg.uploader_ != conn) {
            return results;
        }

        transfer_mgr.cancel_file(file_id);

        // 广播文件失效，房间内所有下载方卡片显示已失效，与退出房间一致
        std::string dwerr = WsFrame::build(WsOpcode::TEXT,
            WsAppParser::build("DWERR", file_id, "上传已取消"));
        broadcast_to_room(room_mgr, conn->get_room_id(), conn->fd_, dwerr, results);

        results.push_back({conn,
            WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("DONE", "cancelled"))});
        return results;
    });

    // ========== DOWNLOAD 处理器 ==========
    // 启动独立窗口传输
    this->on("DOWNLOAD", [](const WsAppMessage& msg,
                             const std::shared_ptr<Connection>& conn,
                             RoomManager& /*room_mgr*/,
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
        // 上传方已离线 文件实际不可下载 与 DWACK 路径一致 不创建传输会话
        if (!reg.uploader_ || !reg.uploader_->alive_) {
            results.push_back({conn,
                WsFrame::build(WsOpcode::TEXT,
                    WsAppParser::build("DWERR", reg.file_id_, "上传方已离开，下载失败"))});
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
        auto init_reqs = transfer_mgr.start_transfer(file_id, conn, start_offset, session_id);
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

        // 上传方在传输启动到发请求之间可能掉线 存活则发 DWREQ 否则告知下载方
        if (auto uploader = reg.uploader_; uploader && uploader->alive_) {
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
        else {
            results.push_back({conn,
                WsFrame::build(WsOpcode::TEXT,
                    WsAppParser::build("DWERR", reg.file_id_, "上传方已离开，下载失败"))});
        }

        return results;
    });

    // ========== DWACK 处理器 ==========
    // 下载方确认收到一个分块，触发下一个 DWREQ
    // 协议: DWACK|<session_id>|<offset>
    this->on("DWACK", [](const WsAppMessage& msg,
                          const std::shared_ptr<Connection>& conn,
                          RoomManager& /*room_mgr*/,
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
            auto uploader = ar.next_->uploader_;
            // 上传方已断开则通知下载方，否则发下一个请求
            if (uploader && uploader->alive_) {
                results.push_back({uploader,
                    WsFrame::build(WsOpcode::TEXT,
                        WsAppParser::build("DWREQ",
                            std::to_string(ar.next_->session_id_),
                            ar.next_->file_id_,
                            std::to_string(ar.next_->offset_),
                            std::to_string(ar.next_->size_)))});
            }
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
        transfer_mgr.cancel_session(msg.param(0), conn);
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
        if (auto target = room_peer(room_mgr, conn->get_room_id(), target_fd)) {
            results.push_back({target, WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("OFFER",
                    std::to_string(conn->fd_), msg.param(1)))});
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
        if (auto target = room_peer(room_mgr, conn->get_room_id(), target_fd)) {
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
        if (auto target = room_peer(room_mgr, conn->get_room_id(), target_fd)) {
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
        if (auto target = room_peer(room_mgr, conn->get_room_id(), target_fd)) {
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
