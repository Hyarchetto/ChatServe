// WebSocket 分片累积状态 — RFC 6455
// 一条消息跨多个帧时 已 consume 但尚未成形的载荷寄存在这里
// 由每连接的持有者保管并传给 WsParser 解析器本身无状态
#pragma once

#include <string>
#include "WsOpcode.h"

struct WsFragmentState {
    bool in_fragmented_ = false;
    WsOpcode first_opcode_ = WsOpcode::TEXT;
    std::string buffer_;
};
