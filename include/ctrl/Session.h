// Session — 连接共享轻量控制块 随消息跨 io/中控/业务线程
// 身份即对象本身 以 shared_ptr 引用计数保活 任务持有期间必不被释放
// io 是 alive_ 唯一写者 关连接时置 false 业务只读
// 同一条会话的业务与收尾都由中控单飞门串行 这里不需要互斥量
// room_ 只有会话自己的操作读写 昵称不在这层 它随成员资格归房间所有
// fd_/io_ 构造后不可变 fd_ 是句柄 io_ 是归属 io 序号 中控分发据此挑邮箱
#pragma once

#include <atomic>
#include <string>

struct Session {
    explicit Session(int fd, int io) : fd_(fd), io_(io) {}

    const int fd_;                    // 句柄
    const int io_;                    // 归属 io 序号 建立连接的 io 构造时填
    std::atomic<bool> alive_{true};   // io 关连接时置 false 业务只读
    std::string room_;                // 所在房间 空表示未加入 退房时清
};
