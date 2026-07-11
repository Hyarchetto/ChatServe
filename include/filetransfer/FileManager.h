// FileManager — 文件上传/下载/元数据管理，线程安全
// 所有文件数据存储在内存中，不写入磁盘
#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <cstdint>

class Connection;

// 文件元数据
struct FileMeta {
    std::string file_id_;
    std::string filename_;
    size_t filesize_ = 0;
    std::string room_id_;
    std::string sender_nickname_;
    std::string data_;          // 完整的文件二进制数据
    bool completed_ = false;
};

// 上传会话
struct UploadSession {
    std::string file_id_;
    std::string filename_;
    size_t filesize_ = 0;
    size_t received_ = 0;
    std::string room_id_;
    std::string sender_nickname_;
    std::string buffer_;        // 上传中的文件数据，不断追加
    int fd_ = -1;
};

// write_chunk 返回值
struct WriteChunkResult {
    size_t received_ = 0;       // 已接收总字节数
    bool completed_ = false;    // 本次调用后上传是否刚完成
    std::string file_id_;       // 完成时的 file_id
};

// 上传完成回调
using UploadCompleteCallback = std::function<void(const FileMeta& meta)>;

class FileManager {
public:
    FileManager();
    ~FileManager();

    // 初始化上传，返回 file_id
    std::string init_upload(const std::string& filename, size_t filesize,
                             const std::string& room_id,
                             const std::string& sender_nickname,
                             int conn_fd);

    // 写入数据块
    // 返回已接收总字节数及是否完成
    WriteChunkResult write_chunk(int conn_fd, const std::string& data);

    // 强制完成上传
    // 如果文件已完成或不存在，返回空 FileMeta
    FileMeta finalize(int conn_fd);

    // 设置上传完成回调
    void set_on_upload_complete(UploadCompleteCallback cb);

    // 读取已完成的文件数据
    bool read_file(const std::string& file_id, std::string& out_data);

    // 查询元数据
    FileMeta get_meta(const std::string& file_id);

    // 取消上传
    void cancel_upload(int conn_fd);

private:
    std::unordered_map<int, UploadSession> sessions_;   // conn_fd → session
    std::unordered_map<std::string, FileMeta> files_;    // file_id → meta
    mutable std::mutex mtx_;

    UploadCompleteCallback on_complete_;

    std::string generate_file_id();
};
