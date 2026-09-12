// 应用层命令路由 — 文件传输域
#include <iostream>
#include <string>
#include <vector>

#include "app/AppRouter.h"
#include "app/AppParser.h"
#include "ctrl/Session.h"

// 一条 DWREQ 请求帧 目标即该请求的上传方 初始窗口与滑动补发共用
static AppRouter::Out dwreq_frame(const NextRequest& req) {
    return {req.uploader_, AppParser::build("DWREQ",
        std::to_string(req.session_id_),
        req.file_id_,
        std::to_string(req.offset_),
        std::to_string(req.size_))};
}

void AppRouter::register_transfer() {
    // ========== UPLOAD 处理器 ==========
    // 仅注册文件元数据，不上传文件内容
    this->on("UPLOAD", [](const AppMessage& msg,
                          std::shared_ptr<Session> session,
                          RoomManager& room_mgr) -> std::vector<Out> {
        std::vector<Out> results;
        if (msg.param_count() < 2) {
            return results;
        }
        // 未加入房间的防御性检查 与 MSG 对齐 防止文件注册到空房间
        std::string room_id = session->room_;
        if (room_id.empty()) {
            return results;
        }

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

        TransferManager& tm = transfer_mgr_of(room_mgr, room_id);
        std::string file_id = tm.register_file(filename, filesize, session);
        if (file_id.empty()) {
            return results;
        }
        // 回复 UPOK 给上传方
        results.push_back({session,
            AppParser::build("UPOK", file_id)});

        // 广播 FILE 通知给房间其他人 末尾带上上传方 fd 作为唯一标识
        std::string notify = AppParser::build("FILE",
            {file_id, filename, std::to_string(filesize),
             std::to_string(session->fd_)});
        broadcast_to_room(room_mgr, room_id, session.get(), notify, results);

        return results;
    });

    // ========== UPCANCEL 处理器 ==========
    // 上传方取消单个文件，通知被孤立的下载方
    this->on("UPCANCEL", [](const AppMessage& msg,
                            std::shared_ptr<Session> session,
                            RoomManager& room_mgr) -> std::vector<Out> {
        std::vector<Out> results;
        if (msg.param_count() < 1) {
            return results;
        }
        std::string file_id = msg.param(0);

        TransferManager& tm = transfer_mgr_of(room_mgr, session->room_);
        // 校验归属：文件存在且属于当前上传方
        auto reg = tm.get_registration(file_id);
        if (reg.file_id_.empty() || reg.uploader_.get() != session.get()) {
            return results;
        }

        tm.cancel_file(file_id);

        // 广播文件失效，房间内所有下载方卡片显示已失效，与退出房间一致
        std::string dwerr = dwerr_frame(file_id, "上传已取消");
        broadcast_to_room(room_mgr, session->room_, session.get(), dwerr, results);

        results.push_back({session,
            AppParser::build("DONE", "cancelled")});
        return results;
    });

    // ========== DOWNLOAD 处理器 ==========
    // 启动独立窗口传输
    this->on("DOWNLOAD", [](const AppMessage& msg,
                            std::shared_ptr<Session> session,
                            RoomManager& room_mgr) -> std::vector<Out> {
        std::vector<Out> results;
        if (msg.param_count() < 1) {
            return results;
        }

        std::string file_id = msg.param(0);
        TransferManager& tm = transfer_mgr_of(room_mgr, session->room_);
        auto reg = tm.get_registration(file_id);
        if (reg.file_id_.empty()) {
            results.push_back({session,
                AppParser::build("SYS", "ERR|文件不存在")});
            return results;
        }
        // 上传方已离线 文件实际不可下载 与 DWACK 路径一致 不创建传输会话
        if (!reg.uploader_ || !reg.uploader_->alive_) {
            results.push_back({session,
                dwerr_frame(reg.file_id_, "上传方已离开，下载失败")});
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
                        << e.what() << " for param '" << msg.param(1) << "'"
                        << std::endl;
            }
        }

        // 启动传输，获取初始窗口请求
        uint64_t session_id = 0;
        auto init_reqs = tm.start_transfer(file_id, session, start_offset,
                                           session_id);
        if (init_reqs.empty()) {
            results.push_back({session,
                AppParser::build("SYS", "ERR|无法启动传输 上传方可能已离线")});
            return results;
        }

        // DWSTART 给下载方
        results.push_back({session,
            AppParser::build("DWSTART",
                reg.file_id_, reg.filename_, std::to_string(reg.filesize_))});

        // 上传方在传输启动到发请求之间可能掉线 存活则发 DWREQ 否则告知下载方
        if (auto uploader = reg.uploader_; uploader && uploader->alive_) {
            for (auto& req : init_reqs) {
                results.push_back(dwreq_frame(req));
            }
        }
        else {
            results.push_back({session,
                dwerr_frame(reg.file_id_, "上传方已离开，下载失败")});
        }

        return results;
    });

    // ========== DWACK 处理器 ==========
    // 下载方确认收到一个分块，触发下一个 DWREQ
    // 协议: DWACK|<session_id>|<offset>
    this->on("DWACK", [](const AppMessage& msg,
                         std::shared_ptr<Session> session,
                         RoomManager& room_mgr) -> std::vector<Out> {
        std::vector<Out> results;
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
                      << " for params [" << msg.param(0) << ", " << msg.param(1)
                      << "]" << std::endl;
            return results;
        }

        TransferManager& tm = transfer_mgr_of(room_mgr, session->room_);
        auto ar = tm.handle_ack(session_id, offset);
        if (!ar.valid_) {
            return results;
        }
        // 该下载方完成，仅通知下载方，上传方文件仍保持可下载
        if (ar.downloader_done_) {
            results.push_back({session,
                AppParser::build("DWNDONE", ar.file_id_)});
        }

        // 发送下一个 DWREQ 给上传方
        if (ar.next_) {
            auto uploader = ar.next_->uploader_;
            // 上传方已断开则通知下载方，否则发下一个请求
            if (uploader && uploader->alive_) {
                results.push_back(dwreq_frame(*ar.next_));
            }
            else {
                results.push_back({session,
                    dwerr_frame(ar.next_->file_id_, "上传方已离开，下载失败")});
            }
        }

        return results;
    });

    // ========== 下载方会话控制 ==========
    // DWNPAUSE 暂停下载，DWNCANCEL 取消下载，都只取消当前会话
    // 协议: DWNPAUSE|<file_id>  /  DWNCANCEL|<file_id>
    auto cancel_session = [](const AppMessage& msg,
                             std::shared_ptr<Session> session,
                             RoomManager& room_mgr) -> std::vector<Out> {
        std::vector<Out> results;
        if (msg.param_count() < 1) {
            return results;
        }
        TransferManager& tm = transfer_mgr_of(room_mgr, session->room_);
        tm.cancel_session(msg.param(0), session);
        return results;
    };
    this->on("DWNPAUSE", cancel_session);
    this->on("DWNCANCEL", cancel_session);
}

// BINARY 分块处理 与旧转发语义一致
std::vector<AppRouter::Out> AppRouter::handle_chunk(
    std::shared_ptr<Session> session, const std::string& data,
    RoomManager& room_mgr) {
    std::vector<Out> results;
    if (session->room_.empty()) {
        return results;
    }
    TransferManager& tm = transfer_mgr_of(room_mgr, session->room_);
    auto result = tm.handle_chunk_data(data);
    if (!result.valid_) {
        return results;
    }
    // 会话持有下载方 Session 断线后 alive_ 为 false 分发时目标已下线则弃帧
    if (auto dl = result.downloader_) {
        std::string dwdata = AppParser::build("DWDATA",
            std::to_string(result.session_id_),
            result.file_id_,
            std::to_string(result.offset_),
            std::to_string(result.size_));
        results.push_back({dl, std::move(dwdata)});
        // 原样中继分块 复用原始载荷免剥头重拼
        Out bin;
        bin.sess_ = std::move(dl);
        bin.text_ = data;
        bin.binary_ = true;
        results.push_back(std::move(bin));
    }
    // 滑动窗口有空位时发送下一个 DWREQ 给上传方
    if (result.next_) {
        if (auto uploader = result.next_->uploader_; uploader && uploader->alive_) {
            results.push_back(dwreq_frame(*result.next_));
        }
    }
    return results;
}
