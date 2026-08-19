// WebSocket 分片累积状态 — RFC 6455
// Connection 包含此头文件用于持有一个 ws_frag_ 成员
// WsParser 在解析分片时也需要它，所以两部分都包含此文件
#pragma once

#include <string>
#include "WsOpcode.h"

struct WsFragmentState {
    bool in_fragmented_ = false;
    WsOpcode first_opcode_ = WsOpcode::TEXT;
    std::string buffer_;
};
