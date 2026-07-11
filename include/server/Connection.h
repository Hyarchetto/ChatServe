// 连接结构体 — 每个 TCP 连接对应一个实例
#pragma once

#include <memory>
#include <string>
#include <functional>

#include <fcntl.h>
#include <unistd.h>

#include "../http/HttpParser.h"
#include "../ws/WebSocketParser.h"

class Connection {
public:
    int fd_;                                    // 套接字 fd
    std::string read_buf_;                      // 累积读取缓冲区
    bool pending_close_ = false;                // 关闭信号

    // ---- WebSocket 状态 ----
    bool ws_mode_ = false;                      // 是否已升级为 WebSocket
    WebSocketFragmentState ws_frag_;            // WebSocket 分片状态

    // ---- 聊天室状态 ----
    std::string room_id_;
    std::string nickname_;
    

    explicit Connection(int fd): fd_(fd){ 
        set_nonblock(); 
    }

    ~Connection(){
        close(this->fd_);
    }

private:
    void set_nonblock() {
        int flags = fcntl(this->fd_, F_GETFL, 0);
        fcntl(this->fd_, F_SETFL, flags | O_NONBLOCK);
    }
};
