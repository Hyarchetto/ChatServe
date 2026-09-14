// 应用层命令路由 — 文件传输域
#include <iostream>
#include <string>
#include <vector>

#include "app/AppRouter.h"
#include "app/AppParser.h"
#include "ctrl/Session.h"

// 一条 DWREQ 请求帧 目标即该请求的上传方 初始窗口与滑动补发共用
static CtrlDown build_dwreq_frame(const NextRequest& req) {
    return {req.uploader_, AppParser::build_frame("DWREQ", std::to_string(req.session_id_),
                                                  req.file_id_, std::to_string(req.offset_),
                                                  std::to_string(req.size_))};
}

// 处理上传方 BINARY 分块 定位会话并转发给下载方
std::vector<CtrlDown> AppRouter::handle_chunk(std::shared_ptr<Session> sess,
                                              const std::string& data) {
    std::vector<CtrlDown> results;
    TransferManager* tm = find_transfer_mgr(sess->room_);
    if (tm == nullptr) {
        return results;
    }
    auto result = tm->handle_chunk_data(sess.get(), data);
    if (!result.valid_) {
        return results;
    }
    // 会话持有下载方 Session 断线后 alive_ 为 false 分发时目标已下线则弃帧
    if (auto dl = result.downloader_) {
        std::string dwdata = AppParser::build_frame("DWDATA",
            std::to_string(result.session_id_),
            result.file_id_,
            std::to_string(result.offset_),
            std::to_string(result.size_));
        results.push_back({dl, std::move(dwdata)});
        // 原样中继分块 复用原始载荷免剥头重拼
        CtrlDown bin;
        bin.sess_ = std::move(dl);
        bin.text_ = data;
        bin.binary_ = true;
        results.push_back(std::move(bin));
    }
    // 滑动窗口有空位时发送下一个 DWREQ 给上传方
    if (result.next_) {
        if (auto uploader = result.next_->uploader_; uploader && uploader->alive_) {
            results.push_back(build_dwreq_frame(*result.next_));
        }
    }
    return results;
}

void AppRouter::register_transfer() {
    // ========== UPLOAD 处理器 ==========
    // 仅注册文件元数据，不上传文件内容
    this->on("UPLOAD", [this](std::shared_ptr<Session> sess,
                              const AppMessage& msg) -> std::vector<CtrlDown> {
        std::vector<CtrlDown> results;
        if (msg.param_count() < 2) {
            return results;
        }
        // 未加入房间则房间查不到 注册与广播自然空转
        std::string room_id = sess->room_;

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

        TransferManager* tm = find_transfer_mgr(room_id);
        if (tm == nullptr) {
            return results;
        }
        std::string file_id = tm->register_file(filename, filesize, sess);
        if (file_id.empty()) {
            return results;
        }
        // 回复 UPOK 给上传方
        results.push_back({sess, AppParser::build_frame("UPOK", file_id)});

        // 广播 FILE 通知给房间其他人 末尾带上上传方 fd 作为唯一标识
        std::string notify = AppParser::build_frame("FILE", {file_id, filename, std::to_string(filesize),
                                                    std::to_string(sess->fd_)});
        this->broadcast_to_room(room_id, sess.get(), notify, results);

        return results;
    });

    // ========== UPCANCEL 处理器 ==========
    // 上传方取消单个文件，通知被孤立的下载方
    this->on("UPCANCEL", [this](std::shared_ptr<Session> sess,
                                const AppMessage& msg) -> std::vector<CtrlDown> {
        std::vector<CtrlDown> results;
        if (msg.param_count() < 1) {
            return results;
        }
        std::string file_id = msg.param(0);
        std::string room_id = sess->room_;

        TransferManager* tm = find_transfer_mgr(room_id);
        if (tm == nullptr) {
            return results;
        }
        // 校验归属：文件存在且属于当前上传方
        auto reg = tm->find_registration(file_id);
        if (!reg || reg->uploader_.get() != sess.get()) {
            return results;
        }

        tm->cancel_file(file_id);

        // 广播文件失效，房间内所有下载方卡片显示已失效，与退出房间一致
        std::string dwerr = build_dwerr_frame(file_id, "上传已取消");
        this->broadcast_to_room(room_id, sess.get(), dwerr, results);

        results.push_back({sess, AppParser::build_frame("DONE", "cancelled")});
        return results;
    });

    // ========== DOWNLOAD 处理器 ==========
    // 启动独立窗口传输
    this->on("DOWNLOAD", [this](std::shared_ptr<Session> sess,
                                const AppMessage& msg) -> std::vector<CtrlDown> {
        std::vector<CtrlDown> results;
        if (msg.param_count() < 1) {
            return results;
        }

        std::string file_id = msg.param(0);
        std::string room_id = sess->room_;
        TransferManager* tm = find_transfer_mgr(room_id);
        if (tm == nullptr) {
            return results;
        }
        auto reg = tm->find_registration(file_id);
        if (!reg) {
            results.push_back({sess, AppParser::build_frame("SYS", "ERR|文件不存在")});
            return results;
        }
        // 上传方已离线 文件实际不可下载 不建传输会话
        if (!reg->uploader_ || !reg->uploader_->alive_) {
            results.push_back({sess, build_dwerr_frame(reg->file_id_, kUploaderGone)});
            return results;
        }

        // 断点续传偏移，普通下载为 0
        size_t start_offset = 0;
        if (msg.param_count() >= 2) {
            try {
                start_offset = std::stoull(msg.param(1));
            }
            catch (const std::exception& e) {
                std::cerr << "DOWNLOAD start_offset parse failed: " << e.what()
                          << " for param '" << msg.param(1) << "'" << std::endl;
                return results;
            }
        }

        // 启动传输，取初始窗口请求，失败时不留下任何会话
        TransferStart start = tm->start_transfer(file_id, sess, start_offset);
        if (!start.valid_) {
            results.push_back({sess, AppParser::build_frame("SYS", "ERR|无法启动传输 上传方可能已离线")});
            return results;
        }

        // DWSTART 给下载方
        results.push_back({sess, AppParser::build_frame("DWSTART", reg->file_id_, reg->filename_,
                                                        std::to_string(reg->filesize_))});

        // 上传方在传输启动到发请求之间可能掉线 存活则发 DWREQ 否则告知下载方
        if (reg->uploader_->alive_) {
            for (auto& req : start.requests_) {
                results.push_back(build_dwreq_frame(req));
            }
        }
        else {
            results.push_back({sess, build_dwerr_frame(reg->file_id_, kUploaderGone)});
        }

        return results;
    });

    // ========== DWACK 处理器 ==========
    // 下载方确认收到一个分块，触发下一个 DWREQ
    // 协议: DWACK|<session_id>|<offset>
    this->on("DWACK", [this](std::shared_ptr<Session> sess,
                             const AppMessage& msg) -> std::vector<CtrlDown> {
        std::vector<CtrlDown> results;
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

        TransferManager* tm = find_transfer_mgr(sess->room_);
        if (tm == nullptr) {
            return results;
        }
        auto ar = tm->handle_ack(sess.get(), session_id, offset);
        if (!ar.valid_) {
            return results;
        }
        // 该下载方全部确认，仅通知下载方，上传方文件仍保持可下载
        if (ar.downloader_done_) {
            results.push_back({sess, AppParser::build_frame("DWNDONE", ar.file_id_)});
        }

        // 窗口补发的下一个 DWREQ
        if (ar.next_) {
            // 上传方已断开则通知下载方，否则发下一个请求
            if (ar.uploader_ && ar.uploader_->alive_) {
                results.push_back(build_dwreq_frame(*ar.next_));
            }
            else {
                results.push_back({sess, build_dwerr_frame(ar.next_->file_id_, kUploaderGone)});
            }
        }

        return results;
    });

    // ========== 下载方会话控制 ==========
    // DWNPAUSE 暂停下载，DWNCANCEL 取消下载，都只取消当前会话
    // 协议: DWNPAUSE|<file_id>  /  DWNCANCEL|<file_id>
    auto cancel_session = [this](std::shared_ptr<Session> sess,
                                 const AppMessage& msg) -> std::vector<CtrlDown> {
        std::vector<CtrlDown> results;
        if (msg.param_count() < 1) {
            return results;
        }
        TransferManager* tm = find_transfer_mgr(sess->room_);
        if (tm == nullptr) {
            return results;
        }
        tm->cancel_session(sess, msg.param(0));
        return results;
    };
    this->on("DWNPAUSE", cancel_session);
    this->on("DWNCANCEL", cancel_session);
}