// 连接结构体 — 每个 TCP 连接对应一个实例
#pragma once

#include <memory>
#include <string>
#include <atomic>
#include <mutex>

#include <fcntl.h>
#include <unistd.h>

#include "../ws/WsFragmentState.h"
#include "LazyBuffer.h"

class WriteScheduler;

class Connection {
public:
    int fd_;                                    // 套接字 fd
    WriteScheduler& writer_;                    // 归属写引擎 内含归属事件循环 发送与投递的目标
    LazyBuffer read_buf_;                       // 累积读取缓冲区
    std::atomic<bool> pending_close_{false};    // 关闭信号
    std::atomic<bool> alive_{true};             // 是否还在连接管理器中

    // ---- WebSocket 状态 ----
    bool ws_mode_ = false;                      // 是否已升级为 WebSocket
    WsFragmentState ws_frag_;                   // WebSocket 分片状态

    // ---- 聊天室状态 线程安全内部 API ----
    void set_identity(const std::string& room, const std::string& nick) {
        std::lock_guard<std::mutex> lock(identity_mtx_);
        room_id_ = room;
        nickname_ = nick;
    }
    std::string get_room_id() const {
        std::lock_guard<std::mutex> lock(identity_mtx_);
        return room_id_;
    }
    std::string get_nickname() const {
        std::lock_guard<std::mutex> lock(identity_mtx_);
        return nickname_;
    }

    // 发送一帧响应到连接 唯一发送入口 线程无关 自动投到连接归属写引擎所在事件循环执行
    // 首参共享指针持有发送期间连接存活 发送缓冲在归属线程消费完才释放
    static void send(const std::shared_ptr<Connection>& conn,
                     std::string data, bool is_high_priority);

    explicit Connection(int fd, WriteScheduler& writer): fd_(fd), writer_(writer){
        set_nonblock();
    }

    ~Connection(){
        if (fd_ >= 0) {
            close(this->fd_);
        }
    }

private:
    void set_nonblock() {
        int flags = fcntl(this->fd_, F_GETFL, 0);
        fcntl(this->fd_, F_SETFL, flags | O_NONBLOCK);
    }

    // ---- 跨线程身份数据 线程池 Worker + IO 线程并发读写 ----
    mutable std::mutex identity_mtx_;
    std::string room_id_;
    std::string nickname_;
};
