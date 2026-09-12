// WsHandler — WebSocket 协议决策器 纯函数不知 io 也不知连接
// 吃缓冲区字节 吐一条决策 施加到连接的动作由 io 层完成
// 分帧委托 WsParser 组帧委托 WsFrame 本层无任何外部依赖
// 与 HttpHandler 同形 一侧解析分帧 一侧解析路由 各自产出待施加决策
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "WsFragmentState.h"

// 一条 WebSocket 处理决策 由施加侧应用到连接
struct WsAction {
    std::vector<std::string> responses_;  // 已组帧线路数据 PONG 与 CLOSE
    std::vector<std::string> messages_;   // 上行文本 交中控
    std::vector<std::string> binaries_;   // 上行二进制分块 交中控
    size_t consumed_ = 0;                 // 已消耗字节 施加侧一次性 consume
    bool close_ = false;                  // 对端发来 CLOSE 置关闭 写引擎冲刷后执行
};

class WsHandler {
public:
    // 分帧并决策缓冲区 返回待施加决策 不碰连接不碰 socket
    // frag 为连接的续帧状态 由调用方按连接提供
    WsAction handle(std::string_view buf, WsFragmentState* frag);
};
