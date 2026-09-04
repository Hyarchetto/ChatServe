// TransferManager — 独立窗口文件传输实现
#include "chatroom/TransferManager.h"

#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>

TransferManager::TransferManager() : rng_(std::random_device{}()) {}

TransferManager::~TransferManager() {}

std::string TransferManager::generate_file_id() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int r = std::uniform_int_distribution<int>(0, 9999)(this->rng_);
    std::ostringstream oss;
    oss << "f_" << ts.tv_sec << ts.tv_nsec << "_" << std::setw(4) << std::setfill('0') << r;
    return oss.str();
}

// ==================== 文件注册 ====================

std::string TransferManager::register_file(const std::string& filename, size_t filesize,
                                             const std::shared_ptr<Connection>& uploader) {
    std::string file_id = this->generate_file_id();
    std::lock_guard<std::mutex> lock(this->mtx_);

    FileRegistration reg;
    reg.file_id_ = file_id;
    reg.filename_ = filename;
    reg.filesize_ = filesize;
    reg.uploader_ = uploader;

    this->registrations_[file_id] = std::move(reg);
    this->uploader_files_[uploader.get()].push_back(file_id);
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
    if (auto it = this->registrations_.find(file_id);
             it != this->registrations_.end()) {
        Connection* uploader = it->second.uploader_.get();
        auto& files = this->uploader_files_[uploader];
        files.erase(std::remove(files.begin(), files.end(), file_id), files.end());
        if (files.empty()){
            this->uploader_files_.erase(uploader);
        }
        this->registrations_.erase(it);
    }
}

// ==================== 启动传输 ====================

std::vector<NextRequest> TransferManager::start_transfer(
    const std::string& file_id, const std::shared_ptr<Connection>& downloader,
    size_t start_offset, uint64_t& out_session_id) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    std::vector<NextRequest> requests;

    out_session_id = 0;

    // 检查文件是否存在
    if (auto r_it = this->registrations_.find(file_id);
             r_it != this->registrations_.end() && r_it->second.uploader_) {

        std::shared_ptr<Connection> uploader = r_it->second.uploader_;
        size_t filesize = r_it->second.filesize_;

        // 创建独立会话
        uint64_t session_id = this->next_session_id_++;
        out_session_id = session_id;

        TransferSession session;
        session.session_id_ = session_id;
        session.file_id_ = file_id;
        session.uploader_ = uploader;
        session.downloader_ = downloader;
        session.filesize_ = filesize;
        session.next_req_offset_ = start_offset;   // 断点续传从该偏移起请求
        session.total_received_ = start_offset;    // 断点前的字节计入完成判定

        // 初始窗口，逐次生成与滑动补发相同的 NextRequest
        size_t total_chunks = (filesize + session.CHUNK_SIZE - 1) / session.CHUNK_SIZE;
        size_t init_count = std::min(session.WINDOW_SIZE, total_chunks);

        for (size_t i = 0; i < init_count; ++i) {
            if (auto req = this->try_send_next_request(session)) {
                requests.push_back(std::move(*req));
            }
            else {
                break;
            }
        }

        this->sessions_[session_id] = std::move(session);
        this->uploader_sessions_[uploader.get()].push_back(session_id);
        this->downloader_sessions_[downloader.get()].push_back(session_id);
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
    if (data.size() != BINARY_HEADER_SIZE + data_size) {
        return result;
    }
    // 查找会话
    auto s_it = this->sessions_.find(session_id);
    if (s_it == this->sessions_.end()) {
        return result;
    }

    TransferSession& ts = s_it->second;
    size_t off = static_cast<size_t>(offset);
    // 偏移越界直接拒绝，否则 chunk_size_for_offset 中 filesize_ - off 无符号下溢
    if (off >= ts.filesize_ || data_size != ts.chunk_size_for_offset(off)) {
        return result;
    }

    // 去重 该偏移仍在窗口内说明已收过
    if (ts.pending_acks_.count(off)) {
        return result;
    }
    ts.pending_acks_.insert(off);
    ts.total_received_ += data_size;

    result.valid_ = true;
    result.session_id_ = session_id;
    result.file_id_ = ts.file_id_;
    result.offset_ = off;
    result.size_ = data_size;
    result.downloader_ = ts.downloader_;

    // 窗口有空位且还有数据未请求时发送下一个 DWREQ
    result.next_ = this->try_send_next_request(ts);

    return result;
}

// ==================== ACK 处理 ====================
std::optional<NextRequest> TransferManager::try_send_next_request(TransferSession& ts) {
    // next_req_offset_ 越过文件末尾即所有块已请求
    if (ts.next_req_offset_ >= ts.filesize_ || !ts.has_window_space()) {
        return std::nullopt;
    }
    size_t req_offset = ts.next_req_offset_;
    size_t req_size = ts.chunk_size_for_offset(req_offset);
    NextRequest next;
    next.session_id_ = ts.session_id_;
    next.file_id_ = ts.file_id_;
    next.offset_ = req_offset;
    next.size_ = req_size;
    next.uploader_ = ts.uploader_;
    ts.next_req_offset_ += req_size;
    return next;
}

AckResult TransferManager::handle_ack(uint64_t session_id, size_t offset) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    AckResult result;

    auto s_it = this->sessions_.find(session_id);
    if (s_it == this->sessions_.end()) {
        return result;
    }

    TransferSession& ts = s_it->second;

    // 移除待确认记录 不在窗口内则非法 ACK
    if (ts.pending_acks_.erase(offset) == 0) {
        return result;
    }

    result.valid_ = true;
    result.session_id_ = session_id;
    result.file_id_ = ts.file_id_;

    if (ts.is_complete()) {
        result.downloader_done_ = true;
        this->cleanup_session_impl(session_id);
        return result;
    }

    // 窗口有空位时发送下一个 DWREQ
    result.next_ = this->try_send_next_request(ts);

    return result;
}

// ==================== 会话清理 ====================

void TransferManager::cleanup_session_impl(uint64_t session_id) {
    auto s_it = this->sessions_.find(session_id);
    if (s_it == this->sessions_.end()) return;

    Connection* uploader = s_it->second.uploader_.get();
    Connection* downloader = s_it->second.downloader_.get();

    // 清理 uploader 和 downloader 索引
    this->remove_session_ref(this->uploader_sessions_, uploader, session_id);
    this->remove_session_ref(this->downloader_sessions_, downloader, session_id);

    this->sessions_.erase(s_it);
}

// 从连接索引中移除会话 id，空则删该条
void TransferManager::remove_session_ref(
    std::unordered_map<Connection*, std::vector<uint64_t>>& map,
    Connection* conn, uint64_t session_id) {
    auto it = map.find(conn);
    if (it == map.end()) {
        return;
    }
    auto& vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), session_id), vec.end());
    if (vec.empty()) {
        map.erase(it);
    }
}

// ==================== 取消传输 ====================

// 清理单个会话，可选记录被孤立的下载方
void TransferManager::cancel_session_impl(uint64_t session_id, CancelResult* result) {
    auto s_it = this->sessions_.find(session_id);
    if (s_it == this->sessions_.end()) {
        return;
    }
    if (result != nullptr) {
        CancelResult::SessionCancel sc;
        sc.file_id_ = s_it->second.file_id_;
        sc.orphaned_downloader_ = s_it->second.downloader_;
        result->cancelled_.push_back(std::move(sc));
    }
    this->cleanup_session_impl(session_id);
}

// 锁内按 file_id + 下载方连接 找最新会话 id，找不到返回 nullopt
std::optional<uint64_t> TransferManager::find_session_id(const std::string& file_id,
                                                         const Connection* downloader) const {
    std::optional<uint64_t> found;
    for (const auto& entry : this->sessions_) {
        const TransferSession& ts = entry.second;
        if (ts.file_id_ == file_id && ts.downloader_.get() == downloader) {
            if (!found || entry.first > *found) {
                found = entry.first;
            }
        }
    }
    return found;
}

// 按文件取消单个文件，返回被孤立的下载方
CancelResult TransferManager::cancel_file(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    CancelResult result;

    auto r_it = this->registrations_.find(file_id);
    if (r_it == this->registrations_.end()) return result;
    Connection* uploader = r_it->second.uploader_.get();

    // 取消该文件的所有活跃下载会话
    auto u_it = this->uploader_sessions_.find(uploader);
    if (u_it != this->uploader_sessions_.end()) {
        auto session_ids = u_it->second;  // 拷贝，循环后 u_it 可能失效
        for (uint64_t sid : session_ids) {
            auto s_it = this->sessions_.find(sid);
            if (s_it != this->sessions_.end() && s_it->second.file_id_ == file_id) {
                this->cancel_session_impl(sid, &result);
            }
        }
    }

    this->unregister_file_impl(file_id);
    return result;
}

CancelResult TransferManager::cancel_by_conn(const std::shared_ptr<Connection>& conn) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    CancelResult result;
    Connection* key = conn.get();

    // 第一阶段：作为上传方取消，记录受影响下载方
    {
        auto r_it = this->uploader_files_.find(key);
        if (r_it != this->uploader_files_.end()) {
            auto file_ids = r_it->second;  // 拷贝，循环后 r_it 可能因 unregister_file_impl 失效
            for (const auto& file_id : file_ids) {
                this->unregister_file_impl(file_id);
            }
        }

        auto u_it = this->uploader_sessions_.find(key);
        if (u_it != this->uploader_sessions_.end()) {
            auto session_ids = u_it->second;  // 拷贝，循环后 u_it 可能失效
            for (uint64_t sid : session_ids) {
                this->cancel_session_impl(sid, &result);
            }
        }
        this->uploader_sessions_.erase(key);  // 按 key，会话清理可能已删掉该条
    }

    // 第二阶段：作为下载方取消
    {
        auto d_it = this->downloader_sessions_.find(key);
        if (d_it != this->downloader_sessions_.end()) {
            auto session_ids = d_it->second;  // 拷贝
            for (uint64_t sid : session_ids) {
                this->cancel_session_impl(sid, nullptr);
            }
        }
        this->downloader_sessions_.erase(key);  // 按 key，会话清理可能已删掉该条
    }

    return result;
}

// ==================== 下载方会话控制 ====================
bool TransferManager::cancel_session(const std::string& file_id,
                                     const std::shared_ptr<Connection>& downloader) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    auto sid = this->find_session_id(file_id, downloader.get());
    if (!sid) {
        return false;
    }
    this->cancel_session_impl(*sid, nullptr);
    return true;
}
