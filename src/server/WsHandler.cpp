// WsHandler — WebSocket 协议处理器
#include "server/WsHandler.h"
#include "server/Connection.h"
#include "server/ConnRegistry.h"
#include "core/EventLoop.h"
#include "core/ThreadPool.h"
#include "ws/WsParser.h"
#include "ws/WsFrame.h"
#include "ws/WsAppRouter.h"
#include "ws/WsAppParser.h"
#include "chatroom/Room.h"
#include "transfer/TransferManager.h"
#include "server/WriteScheduler.h"

WsHandler::WsHandler(EventLoop& loop, ThreadPool& works,
                     WsAppRouter& ws_app_router,
                     RoomManager& room_mgr,
                     TransferManager& transfer_mgr,
                     ConnRegistry& conn_registry,
                     WriteScheduler& writer)
    : loop_(loop), works_(works), ws_app_router_(ws_app_router),
      room_mgr_(room_mgr), transfer_mgr_(transfer_mgr),
      conn_registry_(conn_registry), writer_(writer) {}

void WsHandler::handle_ws(const std::shared_ptr<Connection>& conn) {
    auto ws_result = WsParser::handle(conn->read_buf_, &conn->ws_frag_);
    conn->read_buf_.erase(0, ws_result.consumed_);

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

        // ---- 1. PONG ----
        if (ping) {
            std::string pong = WsFrame::build(WsOpcode::PONG, ping_payload);
            io_actions.push_back([this, conn, pong = std::move(pong)]() {
                this->writer_.push_response(conn, std::move(pong), true);
            });
        }

        // ---- 2. TEXT 路由 ----
        // 每条消息单独转发，route 内部 out = handler() 会替换而非追加
        for (auto& text : msgs) {
            std::vector<WsTargetedMessage> per_msg;
            this->ws_app_router_.route(WsAppParser::parse(text), conn,
                                   this->room_mgr_,
                                   this->transfer_mgr_,
                                   per_msg);
            if (!per_msg.empty()) {
                io_actions.push_back([this, results = std::move(per_msg)]() {
                    for (auto& r : results) {
                        this->writer_.push_response(r.target_, std::move(r.data_), true);
                    }
                });
            }
        }

        // ---- 3. 查是否为传输分块 ----
        for (auto& chunk : binary) {
            auto result = this->transfer_mgr_.handle_chunk_data(chunk);
            if (result.valid_) {
                // 在线程池中立即将 fd 解析为 shared_ptr，避免 IO 线程执行时 fd 被重用
                if (auto dl_conn = this->conn_registry_.get(result.downloader_fd_)) {
                    std::string dwdata = WsFrame::build(WsOpcode::TEXT,
                        WsAppParser::build("DWDATA",
                            std::to_string(result.session_id_),
                            result.file_id_,
                            std::to_string(result.offset_),
                            std::to_string(result.size_)));
                    // 一次构建 BINARY 帧 分块只拷一次 避免 header+chunk 临时串
                    std::string bin_header = TransferManager::make_chunk_header(
                        result.session_id_, result.offset_, result.data_.size());
                    std::string bin = WsFrame::build_from_parts(WsOpcode::BINARY,
                        {bin_header, result.data_});
                    io_actions.push_back([this, dl_conn,
                                          dwdata = std::move(dwdata),
                                          bin = std::move(bin)]() {
                        if (dl_conn->alive_) {
                            this->writer_.push_response(dl_conn, std::move(dwdata), false);
                            this->writer_.push_response(dl_conn, std::move(bin), false);
                        }
                    });
                }
                // 滑动窗口有空位时发送下一个 DWREQ 给上传方
                if (result.next_) {
                    if (auto uploader_conn = this->conn_registry_.get(result.next_->uploader_fd_)) {
                        std::string dwreq = WsFrame::build(WsOpcode::TEXT,
                            WsAppParser::build("DWREQ",
                                std::to_string(result.next_->session_id_),
                                result.next_->file_id_,
                                std::to_string(result.next_->offset_),
                                std::to_string(result.next_->size_)));
                        io_actions.push_back([this, uploader_conn, dwreq = std::move(dwreq)]() {
                            if (uploader_conn->alive_) {
                                this->writer_.push_response(uploader_conn, std::move(dwreq), true);
                            }
                        });
                    }
                }
            }
        }

        // ---- 4. CLOSE 帧 -- 只设关闭标记并回复，清理由 close_connection 分发处理 ----
        if (close) {
            conn->pending_close_ = true;
            std::string close_frame = WsFrame::build(WsOpcode::CLOSE, close_payload);
            this->loop_.run_in_loop([this, conn, close_frame = std::move(close_frame)]() {
                this->writer_.push_response(conn, std::move(close_frame), true);
                this->writer_.flush_responses();
            });
        }

        // ---- 刷新 IO 操作 ----
        if (!io_actions.empty()) {
            this->loop_.run_in_loop([this, actions = std::move(io_actions)]() {
                for (auto& action : actions) {
                    action();
                }
                this->writer_.flush_responses();
            });
        }
    });
}

void WsHandler::cleanup(const std::shared_ptr<Connection>& conn) {
    // WS 专属协议清理 由 Reactor::close_connection 分发调用 只做传输取消房间离开和广播
    this->works_.submit([this, conn, room_id = conn->get_room_id(), nick = conn->get_nickname()]() {
        // 1. 清理传输，通知受影响的下载方
        auto cancel_info = this->transfer_mgr_.cancel_by_fd(conn->fd_);

        std::vector<std::pair<std::shared_ptr<Connection>, std::string>> dwerr_msgs;
        for (auto& sc : cancel_info.cancelled_) {
            std::string dwerr = WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("DWERR", sc.file_id_, "上传方已离开，下载失败"));
            if (auto oc = this->conn_registry_.get(sc.orphaned_downloader_fd_)) {
                dwerr_msgs.push_back({std::move(oc), std::move(dwerr)});
            }
        }

        // 2. 没加入房间则只送 DWERR 后返回
        if (room_id.empty()) {
            if (!dwerr_msgs.empty()) {
                this->loop_.run_in_loop([this, dwerr = std::move(dwerr_msgs)]() {
                    for (auto& [c, msg] : dwerr) {
                        if (c->alive_) this->writer_.push_response(c, msg, true);
                    }
                    this->writer_.flush_responses();
                });
            }
            return;
        }

        // 3. 离开房间
        auto remaining = this->room_mgr_.leave_room(room_id, conn);
        if (remaining.empty() && dwerr_msgs.empty()) return;

        // 4. 构造房间通知
        std::string sys, members_frame;
        if (!remaining.empty()) {
            sys = WsFrame::build(WsOpcode::TEXT,
                WsAppParser::build("SYS", nick + " 离开房间"));

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
            if (auto cc = this->conn_registry_.get(c->fd_)) {
                room_targets.push_back(std::move(cc));
            }
        }

        std::string leave_frame = WsFrame::build(WsOpcode::TEXT,
            WsAppParser::build("LEAVE",
                std::to_string(conn->fd_)));

        this->loop_.run_in_loop([this, dwerr_msgs = std::move(dwerr_msgs),
                                 room_targets = std::move(room_targets),
                                 leave_frame,
                                 sys, members_frame]() {
            for (auto& [c, msg] : dwerr_msgs) {
                if (c->alive_) this->writer_.push_response(c, msg, true);
            }
            for (auto& c : room_targets) {
                if (c->alive_) {
                    this->writer_.push_response(c, leave_frame, true);
                    this->writer_.push_response(c, sys, true);
                    this->writer_.push_response(c, members_frame, true);
                }
            }
            this->writer_.flush_responses();
        });
    });
}
