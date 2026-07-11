// FileManager
// 所有文件数据存储在内存中，不涉及任何磁盘操作
#include "filetransfer/FileManager.h"

#include <sstream>
#include <iomanip>
#include <cstdlib>

#include <ctime>

FileManager::FileManager() {
}

FileManager::~FileManager() {
}

void FileManager::set_on_upload_complete(UploadCompleteCallback cb) {
    this->on_complete_ = std::move(cb);
}

std::string FileManager::generate_file_id() {
    std::lock_guard<std::mutex> lock(this->mtx_);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int r = rand() % 10000;
    std::ostringstream oss;
    oss << "f_" << ts.tv_sec << ts.tv_nsec << "_" << std::setw(4) << std::setfill('0') << r;
    return oss.str();
}

std::string FileManager::init_upload(const std::string& filename, size_t filesize,
                                      const std::string& room_id,
                                      const std::string& sender_nickname,
                                      int conn_fd) {
    std::string file_id = this->generate_file_id();

    UploadSession session;
    session.file_id_ = file_id;
    session.filename_ = filename;
    session.filesize_ = filesize;
    session.received_ = 0;
    session.room_id_ = room_id;
    session.sender_nickname_ = sender_nickname;
    session.fd_ = conn_fd;
    // buffer_ 初始为空，write_chunk 不断追加

    {
        std::lock_guard<std::mutex> lock(this->mtx_);
        this->sessions_[conn_fd] = std::move(session);
    }

    return file_id;
}

WriteChunkResult FileManager::write_chunk(int conn_fd, const std::string& data) {
    {
        std::lock_guard<std::mutex> lock(this->mtx_);
        auto it = this->sessions_.find(conn_fd);
        if (it == this->sessions_.end()) return {};

        it->second.buffer_.append(data);
        it->second.received_ += data.size();
    }

    WriteChunkResult result;
    bool complete = false;
    {
        std::lock_guard<std::mutex> lock(this->mtx_);
        auto it = this->sessions_.find(conn_fd);
        if (it == this->sessions_.end()) return {};

        result.received_ = it->second.received_;
        if (it->second.received_ >= it->second.filesize_) {
            complete = true;
            result.file_id_ = it->second.file_id_;
        }
    }

    if (complete) {
        FileMeta meta = this->finalize(conn_fd);
        result.completed_ = !meta.file_id_.empty();
    }

    return result;
}

FileMeta FileManager::finalize(int conn_fd) {
    UploadSession session;
    {
        std::lock_guard<std::mutex> lock(this->mtx_);
        auto it = this->sessions_.find(conn_fd);
        if (it == this->sessions_.end()) return {};
        session = it->second;
        this->sessions_.erase(it);
    }

    FileMeta meta;
    meta.file_id_ = session.file_id_;
    meta.filename_ = session.filename_;
    meta.filesize_ = session.filesize_;
    meta.room_id_ = session.room_id_;
    meta.sender_nickname_ = session.sender_nickname_;
    meta.data_ = std::move(session.buffer_);
    meta.completed_ = true;

    {
        std::lock_guard<std::mutex> lock(this->mtx_);
        this->files_[session.file_id_] = meta;
    }

    if (this->on_complete_) {
        this->on_complete_(meta);
    }

    return meta;
}

bool FileManager::read_file(const std::string& file_id, std::string& out_data) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    auto it = this->files_.find(file_id);
    if (it == this->files_.end()) return false;
    if (!it->second.completed_) return false;

    out_data = it->second.data_;
    return true;
}

FileMeta FileManager::get_meta(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    auto it = this->files_.find(file_id);
    if (it != this->files_.end()) return it->second;
    return FileMeta{};
}

void FileManager::cancel_upload(int conn_fd) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    auto it = this->sessions_.find(conn_fd);
    if (it == this->sessions_.end()) return;
    // buffer_ 随 session 析构自动释放
    this->sessions_.erase(it);
}
