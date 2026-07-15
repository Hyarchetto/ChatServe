// 独立窗口文件传输
// 每次 DOWNLOAD 创建一个独立 TransferSession，单下载方
// 上传方可注册多个文件，每文件可被多人独立下载，各自独立窗口
// BINARY 帧嵌入 20 字节头部 [session_id:8bytes LE][offset:8bytes LE][data_size:4bytes LE]
// 服务器用 session_id 直接定位传输会话并转发给对应下载方
#pragma once

#include <string>
#include <unordered_map>
#include <map>
#include <set>
#include <mutex>
#include <cstdint>
#include <vector>

// 文件注册
struct FileRegistration {
    std::string file_id_;
    std::string filename_;
    size_t filesize_ = 0;
    std::string room_id_;
    std::string sender_nickname_;
    int uploader_fd_ = -1;
};

// 传输会话，为每次传输都建立一次窗口
struct TransferSession {
    uint64_t session_id_ = 0;
    std::string file_id_;
    int uploader_fd_ = -1;
    int downloader_fd_ = -1;
    size_t filesize_ = 0;
    // 单个文件大小为 256*1024 字节
    static constexpr size_t CHUNK_SIZE = 256 * 1024;
    // 窗口大小为4
    static constexpr size_t WINDOW_SIZE = 4;

    size_t next_req_offset_ = 0;
    bool all_requested_ = false;

    // 已从上传方收到的 payload 字节数，不含 BINARY 帧头
    size_t total_received_ = 0;

    // 上传方已发未确认的数据块，offset -> data
    std::map<size_t, std::string> pending_data_;

    // 该下载方待 ACK 的偏移
    std::set<size_t> pending_acks_;

    // 所有分块已收到且下载方已确认
    // has_window_space 用于判断是否还能发 DWREQ，不参与完成判定
    bool is_complete() const {
        return pending_acks_.empty() && total_received_ >= filesize_;
    }

    size_t chunk_size_for_offset(size_t offset) const {
        return std::min(CHUNK_SIZE, filesize_ - offset);
    }

    bool has_window_space() const {
        return pending_data_.size() < WINDOW_SIZE;
    }
};

// BINARY 帧头部格式: [session_id:8bytes LE][offset:8bytes LE][data_size:4bytes LE]
static constexpr size_t BINARY_HEADER_SIZE = 20;

// handle_chunk_data 返回值
struct ChunkResult {
    bool valid_ = false;
    uint64_t session_id_ = 0;
    std::string file_id_;
    size_t offset_ = 0;
    size_t size_ = 0;
    int downloader_fd_ = -1;
    std::string data_;

    bool has_next_req_ = false;
    uint64_t next_req_session_id_ = 0;
    size_t next_req_offset_ = 0;
    size_t next_req_size_ = 0;
    int next_req_uploader_fd_ = -1;
};

// handle_ack 返回值
struct AckResult {
    bool valid_ = false;
    uint64_t session_id_ = 0;
    bool downloader_done_ = false;
    std::string file_id_;

    bool has_next_req_ = false;
    uint64_t next_req_session_id_ = 0;
    size_t next_req_offset_ = 0;
    size_t next_req_size_ = 0;
    int next_req_uploader_fd_ = -1;
};

// cancel_by_fd 返回值
struct CancelResult {
    struct SessionCancel {
        std::string file_id_;
        int orphaned_downloader_fd_ = -1;
    };
    std::vector<SessionCancel> cancelled_;
    bool any() const { return !cancelled_.empty(); }
};

class TransferManager {
public:
    TransferManager();
    ~TransferManager();

    // 文件注册，同一上传方可注册多个文件
    std::string register_file(const std::string& filename, size_t filesize,
                               const std::string& room_id,
                               const std::string& sender_nickname,
                               int uploader_fd);

    FileRegistration get_registration(const std::string& file_id);
    void unregister_file(const std::string& file_id);
    void unregister_by_uploader(int uploader_fd);

    // 前置 session_id + offset 到 BINARY 帧头部，供前端匹配 DWDATA
    static std::string wrap_chunk_data(uint64_t session_id, size_t offset,
                                        const std::string& chunk);

    // 启动传输，返回初始窗口的请求列表
    // session_id 输出参数，用于后续协议交互
    std::vector<class InitialRequest> start_transfer(
        const std::string& file_id, int downloader_fd, uint64_t& out_session_id);

    // 处理上传方 BINARY 数据，转发给对应下载方
    ChunkResult handle_chunk_data(const std::string& data);

    // 处理下载方 ACK，滑动窗口
    AckResult handle_ack(uint64_t session_id, size_t offset);

    // 按 fd 取消传输，上传方或下载方断开时调用
    CancelResult cancel_by_fd(int fd);

private:
    std::unordered_map<std::string, FileRegistration> registrations_;
    std::unordered_map<uint64_t, TransferSession> sessions_;
    std::unordered_map<int, std::vector<std::string>> uploader_files_;
    std::unordered_map<int, std::vector<uint64_t>> uploader_sessions_;
    std::unordered_map<int, std::vector<uint64_t>> downloader_sessions_;
    uint64_t next_session_id_ = 1;
    mutable std::mutex mtx_;

    std::string generate_file_id();
    void unregister_file_impl(const std::string& file_id);
    void cleanup_session_impl(uint64_t session_id);
    void try_send_next_request(TransferSession& ts, AckResult& result);
};

// 初始窗口请求
struct InitialRequest {
    uint64_t session_id_ = 0;
    std::string file_id_;
    size_t offset_ = 0;
    size_t size_ = 0;
    int uploader_fd_ = -1;
};
