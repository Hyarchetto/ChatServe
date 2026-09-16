// io↔中控 消息 — 以共享 Session 控制块为连接身份
// Session 以 shared_ptr 跨线程携带 引用计数保活 身份即对象 无 fd 复用之虞
// 业务池只摸 Session 不摸 Connection 读缓冲与分片状态留在 io
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Session.h"

// io 上行事件类型 进程内枚举 从不序列化 故取值无含义
enum class CtrlUpKind : uint8_t {
    WS_TEXT,    // 一条完整 WS 文本应用消息 已剥帧 只含消息原文
    CLOSED,     // 连接断开或收到 WS CLOSE 中控清理该连接业务
    WS_BINARY,  // 一条完整 WS 二进制消息 文件分块 payload 含 20B 传输头
};

// io→中控 上行事件
struct CtrlUp {
    CtrlUpKind kind_ = CtrlUpKind::WS_TEXT;
    std::shared_ptr<Session> sess_;     // 连接控制块 归属 io 随其 io_ 自带 中控无需登记
    std::string text_;                  // 应用原文 WS_BINARY 时为原始字节
};

// 中控→io 下行 一条待写给某连接的应用消息 io 按 Session 找连接组帧
// binary 为真时按原始字节组 BINARY 帧 文件分块 否则组 TEXT 帧
struct CtrlDown {
    std::shared_ptr<Session> sess_;
    std::string text_;
    bool binary_ = false;
};
