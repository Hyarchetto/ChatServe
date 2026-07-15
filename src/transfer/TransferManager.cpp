// TransferManager — 独立窗口文件传输实现
#include "transfer/TransferManager.h"

#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <algorithm>

TransferManager::TransferManager() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
}

TransferManager::~TransferManager() {}

std::string TransferManager::generate_file_id() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int r = rand() % 10000;
    std::ostringstream oss;
    oss << "f_" << ts.tv_sec << ts.tv_nsec << "_" << std::setw(4) << std::setfill('0') << r;
    return oss.str();
}

// ==================== 文件注册 ====================

std::string TransferManager::register_file(const std::string& filename, size_t filesize,
                                             const std::string& room_id,
                                             const std::string& sender_nickname,
                                             int uploader_fd) {
    std::string file_id = this->generate_file_id();
    std::lock_guard<std::mutex> lock(this->mtx_);

    FileRegistration reg;
    reg.file_id_ = file_id;
    reg.filename_ = filename;
    reg.filesize_ = filesize;
    reg.room_id_ = room_id;
    reg.sender_nickname_ = sender_nickname;
    reg.uploader_fd_ = uploader_fd;

    this->registrations_[file_id] = std::move(reg);
    this->uploader_files_[uploader_fd].push_back(file_id);
    return file_id;
}

FileRegistration TransferManager::get_registration(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    if (auto it = this->registrations_.find(file_id); it != this->registrations_.end()) {
        return it->second;
    }
    return {};
}

void TransferManager::unregister_file_impl(const std::string& file_id) {
    if (auto it = this->registrations_.find(file_id); it != this->registrations_.end()) {
        int uploader_fd = it->second.uploader_fd_;
        auto& files = this->uploader_files_[uploader_fd];
        files.erase(std::remove(files.begin(), files.end(), file_id), files.end());
        if (files.empty()) this->uploader_files_.erase(uploader_fd);
        this->registrations_.erase(it);
    }
}

void TransferManager::unregister_file(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    this->unregister_file_impl(file_id);
}

void TransferManager::unregister_by_uploader(int uploader_fd) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    // 清理文件注册
    if (auto it = this->uploader_files_.find(uploader_fd); it != this->uploader_files_.end()) {
        auto file_ids = it->second;  // 拷贝，循环后 it 可能因 unregister_file_impl 失效
        for (const auto& file_id : file_ids) {
            this->unregister_file_impl(file_id);
        }
    }
    // 清理该上传方所有活跃传输会话
    if (auto u_it = this->uploader_sessions_.find(uploader_fd); u_it != this->uploader_sessions_.end()) {
        auto session_ids = u_it->second;  // 拷贝
        for (uint64_t sid : session_ids) {
            auto s_it = this->sessions_.find(sid);
            if (s_it != this->sessions_.end()) {
                int dfd = s_it->second.downloader_fd_;
                auto d_it = this->downloader_sessions_.find(dfd);
                if (d_it != this->downloader_sessions_.end()) {
                    auto& vec = d_it->second;
                    vec.erase(std::remove(vec.begin(), vec.end(), sid), vec.end());
                    if (vec.empty()) this->downloader_sessions_.erase(d_it);
                }
                this->sessions_.erase(s_it);
            }
        }
        this->uploader_sessions_.erase(u_it);
    }
}

// ==================== 启动传输 ====================

std::vector<InitialRequest> TransferManager::start_transfer(
    const std::string& file_id, int downloader_fd, uint64_t& out_session_id) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    std::vector<InitialRequest> requests;

    out_session_id = 0;

    // 检查文件是否存在
    if (auto r_it = this->registrations_.find(file_id); 
             r_it != this->registrations_.end() && r_it->second.uploader_fd_ >= 0) {

        int uploader_fd = r_it->second.uploader_fd_;
        size_t filesize = r_it->second.filesize_;

        // 创建独立会话
        uint64_t session_id = this->next_session_id_++;
        out_session_id = session_id;

        TransferSession session;
        session.session_id_ = session_id;
        session.file_id_ = file_id;
        session.uploader_fd_ = uploader_fd;
        session.downloader_fd_ = downloader_fd;
        session.filesize_ = filesize;

        // 初始窗口
        size_t total_chunks = (filesize + session.CHUNK_SIZE - 1) / session.CHUNK_SIZE;
        size_t init_count = std::min(session.WINDOW_SIZE, total_chunks);

        for (size_t i = 0; i < init_count; ++i) {
            size_t offset = session.next_req_offset_;
            size_t size = session.chunk_size_for_offset(offset);
            requests.push_back({session_id, file_id, offset, size, uploader_fd});
            session.next_req_offset_ += size;
        }
        if (session.next_req_offset_ >= filesize) {
            session.all_requested_ = true;
        }

        this->sessions_[session_id] = std::move(session);
        this->uploader_sessions_[uploader_fd].push_back(session_id);
        this->downloader_sessions_[downloader_fd].push_back(session_id);
    }
    return requests;
}

// ==================== 二进制分块处理 ====================

ChunkResult TransferManager::handle_chunk_data(const std::string& data) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    ChunkResult result;

    // 头部: [session_id:8bytes LE][offset:8bytes LE][data_size:4bytes LE]
    if (data.size() < BINARY_HEADER_SIZE) return result;

    uint64_t session_id = 0;
    std::memcpy(&session_id, data.data(), 8);
    uint64_t offset = 0;
    uint32_t data_size = 0;
    std::memcpy(&offset, data.data() + 8, 8);
    std::memcpy(&data_size, data.data() + 16, 4);
    if (data.size() != BINARY_HEADER_SIZE + data_size) return result;

    // 查找会话
    auto s_it = this->sessions_.find(session_id);
    if (s_it == this->sessions_.end()) return result;
    TransferSession& ts = s_it->second;

    size_t off = static_cast<size_t>(offset);
    if (data_size != ts.chunk_size_for_offset(off)) return result;

    // 去重
    if (ts.pending_data_.count(off)) return result;

    std::string chunk_data(data.data() + BINARY_HEADER_SIZE, data_size);
    ts.pending_data_[off] = chunk_data;
    ts.pending_acks_.insert(off);
    ts.total_received_ += data_size;

    result.valid_ = true;
    result.session_id_ = session_id;
    result.file_id_ = ts.file_id_;
    result.offset_ = off;
    result.size_ = data_size;
    result.downloader_fd_ = ts.downloader_fd_;
    result.data_ = std::move(chunk_data);

    // 窗口有空位且还有数据未请求时发送下一个 DWREQ
    if (ts.has_window_space() && !ts.all_requested_) {
        size_t req_offset = ts.next_req_offset_;
        size_t req_size = ts.chunk_size_for_offset(req_offset);
        result.has_next_req_ = true;
        result.next_req_session_id_ = session_id;
        result.next_req_offset_ = req_offset;
        result.next_req_size_ = req_size;
        result.next_req_uploader_fd_ = ts.uploader_fd_;
        ts.next_req_offset_ += req_size;
        if (ts.next_req_offset_ >= ts.filesize_) {
            ts.all_requested_ = true;
        }
    }

    return result;
}

// ==================== 分块转发包装 ====================

std::string TransferManager::wrap_chunk_data(uint64_t session_id, size_t offset,
                                               const std::string& chunk) {
    std::string header(BINARY_HEADER_SIZE, '\0');
    std::memcpy(&header[0], &session_id, 8);
    uint64_t off = offset;
    std::memcpy(&header[8], &off, 8);
    uint32_t sz = static_cast<uint32_t>(chunk.size());
    std::memcpy(&header[16], &sz, 4);
    return header + chunk;
}

// ==================== ACK 处理 ====================

void TransferManager::try_send_next_request(TransferSession& ts, AckResult& result) {
    if (ts.has_window_space() && !ts.all_requested_) {
        size_t req_offset = ts.next_req_offset_;
        size_t req_size = ts.chunk_size_for_offset(req_offset);
        result.has_next_req_ = true;
        result.next_req_session_id_ = ts.session_id_;
        result.next_req_offset_ = req_offset;
        result.next_req_size_ = req_size;
        result.next_req_uploader_fd_ = ts.uploader_fd_;
        ts.next_req_offset_ += req_size;
        if (ts.next_req_offset_ >= ts.filesize_) {
            ts.all_requested_ = true;
        }
    }
}

AckResult TransferManager::handle_ack(uint64_t session_id, size_t offset) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    AckResult result;

    auto s_it = this->sessions_.find(session_id);
    if (s_it == this->sessions_.end()) {
        return result;
    }

    TransferSession& ts = s_it->second;

    // 移除待确认记录
    if (ts.pending_acks_.erase(offset) == 0) {
        return result;  
    }

    ts.pending_data_.erase(offset);

    result.valid_ = true;
    result.session_id_ = session_id;
    result.file_id_ = ts.file_id_;

    if (ts.is_complete()) {
        result.downloader_done_ = true;
        this->cleanup_session_impl(session_id);
        return result;
    }

    // 窗口有空位时发送下一个 DWREQ
    try_send_next_request(ts, result);

    return result;
}

// ==================== 会话清理 ====================

void TransferManager::cleanup_session_impl(uint64_t session_id) {
    auto s_it = this->sessions_.find(session_id);
    if (s_it == this->sessions_.end()) return;

    int uploader_fd = s_it->second.uploader_fd_;
    int downloader_fd = s_it->second.downloader_fd_;

    // 清理 uploader 索引
    auto u_it = this->uploader_sessions_.find(uploader_fd);
    if (u_it != this->uploader_sessions_.end()) {
        auto& vec = u_it->second;
        vec.erase(std::remove(vec.begin(), vec.end(), session_id), vec.end());
        if (vec.empty()) {
            this->uploader_sessions_.erase(u_it);
        }
    }

    // 清理 downloader 索引
    auto d_it = this->downloader_sessions_.find(downloader_fd);
    if (d_it != this->downloader_sessions_.end()) {
        auto& vec = d_it->second;
        vec.erase(std::remove(vec.begin(), vec.end(), session_id), vec.end());
        if (vec.empty()) {
            this->downloader_sessions_.erase(d_it);
        }
    }

    this->sessions_.erase(s_it);
}

// ==================== 取消传输 ====================

CancelResult TransferManager::cancel_by_fd(int fd) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    CancelResult result;

    // 第一阶段：作为上传方取消，记录受影响下载方
    {
        // 清理文件注册
        auto r_it = this->uploader_files_.find(fd);
        if (r_it != this->uploader_files_.end()) {
            auto file_ids = r_it->second;  // 拷贝，循环后 r_it 可能因 unregister_file_impl 失效
            for (const auto& file_id : file_ids) {
                this->unregister_file_impl(file_id);
            }
        }

        // 清理活跃传输会话
        auto u_it = this->uploader_sessions_.find(fd);
        if (u_it != this->uploader_sessions_.end()) {
            auto session_ids = u_it->second;  // 拷贝
            for (uint64_t sid : session_ids) {
                auto s_it = this->sessions_.find(sid);
                if (s_it != this->sessions_.end()) {
                    CancelResult::SessionCancel sc;
                    sc.file_id_ = s_it->second.file_id_;
                    sc.orphaned_downloader_fd_ = s_it->second.downloader_fd_;
                    result.cancelled_.push_back(std::move(sc));

                    // 清理 downloader 索引
                    int dfd = s_it->second.downloader_fd_;
                    auto d_it = this->downloader_sessions_.find(dfd);
                    if (d_it != this->downloader_sessions_.end()) {
                        auto& vec = d_it->second;
                        vec.erase(std::remove(vec.begin(), vec.end(), sid), vec.end());
                        if (vec.empty()) {
                            this->downloader_sessions_.erase(d_it);
                        }
                    }

                    this->sessions_.erase(s_it);
                }
            }
            this->uploader_sessions_.erase(u_it);
        }
    }

    // 第二阶段：作为下载方取消
    {
        auto d_it = this->downloader_sessions_.find(fd);
        if (d_it != this->downloader_sessions_.end()) {
            auto session_ids = d_it->second;  // 拷贝
            for (uint64_t sid : session_ids) {
                auto s_it = this->sessions_.find(sid);
                if (s_it != this->sessions_.end()) {
                    int ufd = s_it->second.uploader_fd_;

                    auto u_it = this->uploader_sessions_.find(ufd);
                    if (u_it != this->uploader_sessions_.end()) {
                        auto& vec = u_it->second;
                        vec.erase(std::remove(vec.begin(), vec.end(), sid), vec.end());
                        if (vec.empty()) {
                            this->uploader_sessions_.erase(u_it);
                        }
                    }

                    this->sessions_.erase(s_it);
                }
            }
            this->downloader_sessions_.erase(d_it);
        }
    }

    return result;
}
