// Session — 连接共享轻量控制块 随消息跨 io/中控/业务线程
// 身份即对象本身 以 shared_ptr 引用计数保活 任务持有期间必不被释放
// io 是 alive_/close_ 唯一写者(关连接置 false/收到 WS CLOSE 置位) 业务只读
// room_/nick_ 属业务态 读写经中控业务锁 只被业务池与中控触碰
// fd_/io_ 构造后不可变 fd_ 是线号 io_ 是归属 io 序号 中控分发据此挑频道
#pragma once

#include <atomic>
#include <string>

struct Session {
    explicit Session(int fd, int io) : fd_(fd), io_(io) {}

    const int fd_;                    // 线号 拼应用协议 payload 用
    const int io_;                    // 归属 io 序号 建立连接的 io 构造时填 此后不变
    std::atomic<bool> alive_{true};   // io 关连接时置 false 业务只读
    std::atomic<bool> close_{false};  // io 收到 WS CLOSE 置位 由 io 执行关闭
    std::string room_;                // 所在房间 空表示未加入
    std::string nick_;                // 昵称
};
