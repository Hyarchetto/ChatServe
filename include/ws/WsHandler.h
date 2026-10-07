// WsHandler — WebSocket 协议决策器
// 吃缓冲区字节，吐一条决策
// 循环分帧直到凑不齐一帧，分帧委托 WsParser，组帧委托 WsFrame
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "WsFragmentState.h"

// 一条 WebSocket 处理决策，由施加侧应用到连接
struct WsAction {
    std::vector<std::string> responses_;  // 已组帧线路数据 PONG 与 CLOSE
    std::vector<std::string> messages_;   // 上行文本，交中控
    std::vector<std::string> binaries_;   // 上行二进制分块，交中控
    size_t consumed_ = 0;                 // 已消耗字节，施加侧一次性 consume
    bool close_ = false;                  // 关闭意图，写引擎冲刷后执行
};

class WsHandler {
public:
    // 分帧并决策缓冲区，返回待施加决策
    // frag 为连接的续帧状态，由调用方按连接提供
    WsAction handle(std::string_view buf, WsFragmentState& frag) const;
};
