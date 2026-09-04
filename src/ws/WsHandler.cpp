// WsHandler — WebSocket 协议处理器
#include "ws/WsHandler.h"
#include "conn/Connection.h"
#include "core/EventLoop.h"
#include "core/ThreadPool.h"
#include "ws/WsParser.h"
#include "ws/WsFrame.h"
#include "ws/WsAppRouter.h"
#include "ws/WsAppParser.h"
#include "chatroom/Room.h"
#include "chatroom/TransferManager.h"

WsHandler::WsHandler(EventLoop& loop, ThreadPool& works,
                     RoomManager& room_mgr)
    : loop_(loop), works_(works), room_mgr_(room_mgr) {}

TransferManager& WsHandler::transfer_mgr_of(const std::string& room_id) {
    // 传输状态归房间 从房间取 每个房间独立
    return this->room_mgr_.get_or_create(room_id)->transfer_mgr();
}

void WsHandler::handle_ws(const std::shared_ptr<Connection>& conn) {
    auto ws_result = WsParser::handle({conn->read_buf_.data(), conn->read_buf_.size()},
                                      &conn->ws_frag_);
    conn->read_buf_.consume(ws_result.consumed_);

    if (ws_result.binary_messages_.empty() && 
        ws_result.messages_.empty() && 
        !(ws_result.ping_ || ws_result.close_)) return; 

    this->works_.submit([this, conn,
                         binary = std::move(ws_result.binary_messages_),
                         msgs = std::move(ws_result.messages_),
                         ping = ws_result.ping_,
                         ping_payload = std::move(ws_result.ping_payload_),
                         close = ws_result.close_,
                         close_payload = std::move(ws_result.close_payload_)]() {
        std::vector<std::function<void()>> io_actions;
        // 传输状态归 conn 所在房间 本批消息共享同一房间的传输管理器
        TransferManager& tm = this->transfer_mgr_of(conn->get_room_id());

        // ---- 1. PONG ----
        if (ping) {
            std::string pong = WsFrame::build(WsOpcode::PONG, ping_payload);
            io_actions.push_back([conn, pong = std::move(pong)]() mutable {
                Connection::send(conn, std::move(pong), true);
            });
        }

        // ---- 2. TEXT 路由 ----
        // 每条消息单独转发，handle 内部 out = handler() 会替换而非追加
        for (auto& text : msgs) {
            std::vector<WsTargetedMessage> per_msg;
            this->ws_app_router_.handle(WsAppParser::parse(text), conn,
                                   this->room_mgr_, tm, per_msg);
            if (!per_msg.empty()) {
                io_actions.push_back([results = std::move(per_msg)]() {
                    for (auto& r : results) {
                        Connection::send(r.target_, std::move(r.data_), true);
                    }
                });
            }
        }

        // ---- 3. 查是否为传输分块 ----
        for (auto& chunk : binary) {
            auto result = tm.handle_chunk_data(chunk);
            if (result.valid_) {
                // 会话持有下载方连接 断线后 alive_ 为 false 由写调度发送时过滤
                if (auto dl_conn = result.downloader_) {
                    std::string dwdata = WsFrame::build(WsOpcode::TEXT,
                        WsAppParser::build("DWDATA",
                            std::to_string(result.session_id_),
                            result.file_id_,
                            std::to_string(result.offset_),
                            std::to_string(result.size_)));
                    // 上传方 BINARY 载荷 [20B 头][分块] 已在 handle_chunk_data 校验 直接中继 省剥头重拼
                    std::string bin = WsFrame::build_from_parts(WsOpcode::BINARY, {chunk});
                    io_actions.push_back([dl_conn,
                                          dwdata = std::move(dwdata),
                                          bin = std::move(bin)]() mutable {
                        Connection::send(dl_conn, std::move(dwdata), false);
                        Connection::send(dl_conn, std::move(bin), false);
                    });
                }
                // 滑动窗口有空位时发送下一个 DWREQ 给上传方
                if (result.next_) {
                    if (auto uploader_conn = result.next_->uploader_) {
                        std::string dwreq = WsFrame::build(WsOpcode::TEXT,
                            WsAppParser::build("DWREQ",
                                std::to_string(result.next_->session_id_),
                                result.next_->file_id_,
                                std::to_string(result.next_->offset_),
                                std::to_string(result.next_->size_)));
                        io_actions.push_back([uploader_conn, dwreq = std::move(dwreq)]() mutable {
                            Connection::send(uploader_conn, std::move(dwreq), true);
                        });
                    }
                }
            }
        }

        // ---- 4. CLOSE 帧 -- 只设关闭标记并回复，清理由 close_connection 分发处理 ----
        if (close) {
            conn->pending_close_ = true;
            std::string close_frame = WsFrame::build(WsOpcode::CLOSE, close_payload);
            this->loop_.run_in_loop([this, conn, close_frame = std::move(close_frame)]() mutable {
                Connection::send(conn, std::move(close_frame), true);
            });
        }

        // ---- 刷新 IO 操作 ----
        if (!io_actions.empty()) {
            this->loop_.run_in_loop([this, actions = std::move(io_actions)]() {
                for (auto& action : actions) {
                    action();
                }
            });
        }
    });
}

void WsHandler::cleanup(const std::shared_ptr<Connection>& conn) {
    // WS 专属协议清理 由 ConnHandler::close_connection 分发调用 只做传输取消房间离开和广播
    this->works_.submit([this, conn, room_id = conn->get_room_id()]() {
        // 1. 清理传输，通知受影响的下载方
        TransferManager& tm = this->transfer_mgr_of(room_id);
        auto cancel_info = tm.cancel_by_conn(conn);

        std::vector<std::pair<std::shared_ptr<Connection>, std::string>> dwerr_msgs;
        for (auto& sc : cancel_info.cancelled_) {
            std::string dwerr = WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("DWERR", sc.file_id_, "上传方已离开，下载失败"));
            if (auto oc = sc.orphaned_downloader_) {
                dwerr_msgs.push_back({std::move(oc), std::move(dwerr)});
            }
        }

        // 2. 没加入房间则只送 DWERR 后返回
        if (room_id.empty()) {
            if (!dwerr_msgs.empty()) {
                this->loop_.run_in_loop([this, dwerr = std::move(dwerr_msgs)]() mutable {
                    for (auto& [c, msg] : dwerr) {
                        Connection::send(c, std::move(msg), true);
                    }
                });
            }
            return;
        }

        // 3. 离开房间
        auto remaining = this->room_mgr_.leave_room(room_id, conn);
        if (remaining.empty() && dwerr_msgs.empty()) return;

        // 4. 构造房间通知 只更新 MEMBERS 离开事件由 LEAVE 带 fd 客户端自行解析昵称
        std::string members_frame;
        if (!remaining.empty()) {
            std::string joined;
            for (size_t i = 0; i < remaining.size(); ++i) {
                if (i > 0) joined += ",";
                joined += std::to_string(remaining[i]->fd_) + ":" + remaining[i]->get_nickname();
            }
            members_frame = WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("MEMBERS", joined));
        }

        std::vector<std::shared_ptr<Connection>> room_targets;
        for (auto& c : remaining) {
            if (c->alive_) {
                room_targets.push_back(c);
            }
        }

        std::string leave_frame = WsFrame::build(WsOpcode::TEXT,
            WsAppParser::build("LEAVE",
                std::to_string(conn->fd_)));

        this->loop_.run_in_loop([this, dwerr_msgs = std::move(dwerr_msgs),
                                 room_targets = std::move(room_targets),
                                 leave_frame,
                                 members_frame]() mutable {
            for (auto& [c, msg] : dwerr_msgs) {
                Connection::send(c, std::move(msg), true);
            }
            for (auto& c : room_targets) {
                Connection::send(c, leave_frame, true);
                Connection::send(c, members_frame, true);
            }
        });
    });
}
