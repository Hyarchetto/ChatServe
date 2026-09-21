// 独立窗口文件传输
// 每次 DOWNLOAD 创建一个独立 TransferSession，单下载方
// 上传方可注册多个文件，每文件可被多人独立下载，各自独立窗口
// BINARY 帧嵌入 20 字节头部 [session_id:8bytes LE][offset:8bytes LE][data_size:4bytes LE]
// 服务器用 session_id 直接定位传输会话并转发给对应下载方
#pragma once

#include <string>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <mutex>
#include <cstdint>
#include <vector>
#include <random>
#include <optional>
#include <memory>

#include "../ctrl/Session.h"

// 文件注册
struct FileRegistration {
    std::string file_id_;                           // 文件句柄
    std::string filename_;                          // 文件名
    size_t filesize_ = 0;                           // 文件大小
    std::shared_ptr<Session> uploader_;          // 上传者连接
};

// 传输会话，为每次传输都建立一次窗口
struct TransferSession {
    uint64_t session_id_ = 0;                                   // 会话标识           
    std::string file_id_;                                       // 文件句柄
    std::shared_ptr<Session> uploader_;                      // 上传者连接
    std::shared_ptr<Session> downloader_;                    // 下载者连接
    size_t filesize_ = 0;                                       // 文件大小
    
    static constexpr size_t kChunkSize = 256 * 1024;            // 单个文件块大小为 256*1024 字节
    static constexpr size_t kWindowSize = 8;                    // 窗口大小为8

    size_t next_req_offset_ = 0;                                // 下一个要请求的偏移量，>= filesize_ 表示所有块已请求

    size_t total_received_ = 0;                                 // 已从上传方收到的 payload 字节数

    std::set<size_t> pending_acks_;                             // 在途未确认的偏移 兼做去重与窗口占用

    // 是否所有分块已收到且下载方已确认
    bool is_complete() const {
        return pending_acks_.empty() && total_received_ >= filesize_;
    }

    // 该偏移处分块的字节数，末块可能不足 kChunkSize
    size_t chunk_size_for_offset(size_t offset) const {
        return std::min(kChunkSize, filesize_ - offset);
    }

    // 窗口是否还有空位发送下一个 DWREQ
    bool has_window_space() const {
        return pending_acks_.size() < kWindowSize;
    }
};

// BINARY 帧头部格式: [session_id:8bytes LE][offset:8bytes LE][data_size:4bytes LE]
static constexpr size_t kBinaryHeaderSize = 20;

// 发往上传方的 DWREQ 请求，初始窗口与滑动补发共用同一结构
struct NextRequest {
    uint64_t session_id_ = 0;        // 会话标识
    std::string file_id_;            // 文件句柄
    size_t offset_ = 0;              // 请求分块的文件内偏移
    size_t size_ = 0;                // 请求分块的字节数
    std::shared_ptr<Session> uploader_;       // 目标上传方连接
};

// start_transfer 返回值，无效时调用方回错误且不留会话
struct TransferStart {
    bool valid_ = false;                    // 传输已建立，可以开始发请求
    uint64_t session_id_ = 0;               // 会话标识
    std::vector<NextRequest> requests_;     // 初始窗口的 DWREQ 请求
};

// handle_chunk_data 返回值
struct ChunkResult {
    bool valid_ = false;                     // 数据合法且会话存在
    uint64_t session_id_ = 0;                // 所属会话标识
    std::string file_id_;                    // 所属文件句柄
    size_t offset_ = 0;                      // 分块在文件内的偏移
    size_t size_ = 0;                        // 分块字节数
    std::shared_ptr<Session> downloader_;   // 目标下载方连接
    // 窗口补发的下一个 DWREQ，无则 nullopt
    std::optional<NextRequest> next_;
};

// handle_ack 返回值
struct AckResult {
    bool valid_ = false;                     // ACK 匹配未确认分块时有效
    uint64_t session_id_ = 0;                // 所属会话标识
    bool downloader_done_ = false;           // 该下载方全部分块已确认，传输完成
    std::string file_id_;                    // 所属文件句柄
    // 窗口补发的下一个 DWREQ，无则 nullopt
    std::optional<NextRequest> next_;
    // 会话结束时要用的信息，完成时会话已被清掉取不到
    std::shared_ptr<Session> uploader_;      // 上传方连接
};

// cancel_by_session 返回值
struct CancelResult {
    struct SessionCancel {
        std::string file_id_;                 // 被取消传输的文件句柄
        std::shared_ptr<Session> orphaned_downloader_;  // 因上传方断开而中断的下载方连接
    };
    std::vector<SessionCancel> cancelled_;    // 受影响会话列表，供调用方通知下载方
};

class TransferManager {
public:
    // 构造函数
    TransferManager();
    // 析构函数
    ~TransferManager();

    // 文件注册，同一上传方可注册多个文件，零字节文件拒绝并返回空串
    std::string register_file(const std::string& filename, size_t filesize,
                               const std::shared_ptr<Session>& uploader);

    // 查询文件注册信息，file_id 不存在时返回 nullopt
    std::optional<FileRegistration> find_registration(const std::string& file_id);

    // 启动传输，返回初始窗口的请求列表
    // start_offset 为断点续传的起始偏移，普通下载传 0
    // 上传方已失效或起始偏移越界都返回无效结果，此时不建会话
    TransferStart start_transfer(const std::string& file_id,
                                 const std::shared_ptr<Session>& downloader,
                                 size_t start_offset);

    // 处理 BINARY 数据，from 为发送方，非本会话上传方一律丢弃
    ChunkResult handle_chunk_data(const Session* from, const std::string& data);

    // 处理 ACK，from 为发送方，非本会话下载方一律丢弃
    AckResult handle_ack(const Session* from, uint64_t session_id, size_t offset);

    // 按连接取消传输，上传方或下载方断开时调用
    CancelResult cancel_by_session(const std::shared_ptr<Session>& sess);

    // 按文件取消单个文件，返回被孤立的下载方
    CancelResult cancel_file(const std::string& file_id);

    // 取消会话，仅清会话不碰文件注册，返回是否找到会话
    bool cancel_session(const std::shared_ptr<Session>& downloader,
                        const std::string& file_id);

private:
    std::unordered_map<std::string, FileRegistration> registrations_;           // 文件注册表，file_id -> 注册信息
    std::unordered_map<uint64_t, TransferSession> sessions_;                    // 活跃传输会话表，session_id -> 会话
    // 身份索引键为连接对象指针 不持有所有权 连接由注册和会话持有
    // 用指针而非 fd 标识对端 连接断开后即使 fd 被重用也不会误命中
    std::unordered_map<Session*, std::vector<std::string>> uploader_files_;   // 上传方 -> 其注册文件句柄列表
    std::unordered_map<Session*, std::vector<uint64_t>> uploader_sessions_;   // 上传方 -> 其活跃会话 id 列表
    std::unordered_map<Session*, std::vector<uint64_t>> downloader_sessions_; // 下载方 -> 其活跃会话 id 列表
    uint64_t next_session_id_ = 1;                                              // 会话 id 自增计数器
    std::mt19937 rng_;                                                          // 文件句柄随机数发生器
    mutable std::mutex mtx_;                                                    // 保护以上数据结构的互斥锁

    // 生成文件句柄
    std::string create_file_id();
    // 生成不与已有注册冲突的文件句柄 调用方须已持锁
    std::string create_unique_file_id();
    // 文件注销
    void unregister_file(const std::string& file_id);
    // 会话清理
    void cleanup_session(uint64_t session_id);
    // 取消单个会话
    void cancel_session_impl(uint64_t session_id, CancelResult* result);
    // 锁内查找 file_id + 下载方连接 的最新会话 id，找不到返回 nullopt
    std::optional<uint64_t> find_session_id(const std::string& file_id,
                                            const Session* downloader) const;
    // 窗口有空位时生成下一个 DWREQ
    std::optional<NextRequest> try_send_next_request(TransferSession& ts);
    // 从连接索引中移除会话 id，空则删该条
    void remove_session_ref(std::unordered_map<Session*, std::vector<uint64_t>>& map,
                            Session* sess, uint64_t session_id);
};
