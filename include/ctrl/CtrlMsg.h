// 跨线程消息 — io↔中控与中控↔业务池，以共享 Session 控制块为连接身份
// Session 以 shared_ptr 跨线程携带，引用计数保活，身份即对象，无 fd 复用之虞
// 业务池只摸 Session，读缓冲与分片状态留在 io
// 两个方向的载荷类型各成枚举，都止于应用层
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Session.h"

// 上行事件类型，进程内枚举，从不序列化，故取值无含义
enum class CtrlUpKind : uint8_t {
    WS_TEXT,    // 一条完整 WS 文本应用消息，已剥帧，只含消息原文
    WS_BINARY,  // 一条完整 WS 二进制消息，文件分块 payload 含 20B 传输头
    CLOSED,     // 连接断开或收到 WS CLOSE，中控清理该连接业务
};

// io→中控的上行事件，中控调度后交业务池处理
// CLOSED 由 io 上报，中控在在途业务结束时补一条收尾
struct CtrlUp {
    std::shared_ptr<Session> sess_;     // 连接控制块，归属 io 随其 io_ 自带，中控无需登记
    std::string text_;                  // 应用原文 WS_BINARY 时为原始字节，收尾时为空
    CtrlUpKind kind_ = CtrlUpKind::WS_TEXT;
};

// 中控→io 下行载荷类型
// 只到「文本还是原始字节」为止，组哪种帧由 io 侧决定
enum class CtrlDownKind : uint8_t {
    WS_TEXT,    // 文本应用消息
    WS_BINARY,  // 原始字节，文件分块
};

// 中控→io 下行，一条待写给某连接的应用消息 io 按 Session 找连接组帧
struct CtrlDown {
    std::shared_ptr<Session> sess_;     // 目标连接
    std::string text_;                  // 应用原文或分块原始字节
    CtrlDownKind kind_ = CtrlDownKind::WS_TEXT;
};

// 池→中控，一条算完的业务响应，携带会话保活
struct CtrlResult {
    std::shared_ptr<Session> sess_;
    std::vector<CtrlDown> frames_;
};
