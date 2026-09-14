// WebSocket 分片累积状态 — RFC 6455
// 一条消息跨多个帧时 已 consume 但尚未成形的载荷寄存在这里
// 由每连接的持有者保管并传给 WsParser 解析器本身无状态
#pragma once

#include <string>
#include "WsOpcode.h"

struct WsFragmentState {
    // 一条消息跨帧累积的上限，单帧上限管不住拆成很多帧的情况
    // 收到超过上限的 CONTINUATION 时解析器置 close_ 不再接收
    static constexpr size_t kMaxMessageBytes = 8 * 1024 * 1024;

    bool in_fragmented_ = false;
    WsOpcode first_opcode_ = WsOpcode::TEXT;
    std::string buffer_;
};
