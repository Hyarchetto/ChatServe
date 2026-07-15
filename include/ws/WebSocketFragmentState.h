// WebSocket 分片累积状态 — RFC 6455
// Connection 包含此头文件用于持有一个 ws_frag_ 成员
// WebSocketParser 在解析分片时也需要它，所以两部分都包含此文件
#pragma once

#include <string>
#include "WebSocketOpcode.h"

struct WebSocketFragmentState {
    bool in_fragmented_ = false;
    WebSocketOpcode first_opcode_ = WebSocketOpcode::TEXT;
    std::string buffer_;
};
