// WsHandler — WebSocket 协议决策器 纯函数实现
#include "ws/WsHandler.h"

#include <utility>

#include "ws/WsParser.h"
#include "ws/WsFrame.h"
#include "ws/WsOpcode.h"

WsAction WsHandler::handle(std::string_view buf, WsFragmentState* frag) {
    WsAction action;
    WsResult result = WsParser::handle(buf, frag);
    action.consumed_ = result.consumed_;

    // 无完整消息也无控制帧 只有半帧 消耗已带走 无待发动作
    if (result.binary_messages_.empty() && result.messages_.empty() &&
        !result.ping_ && !result.close_) {
        return action;
    }

    // PING 本地回 PONG
    if (result.ping_) {
        action.responses_.push_back(WsFrame::build(WsOpcode::PONG, result.ping_payload_));
    }
    // 完整 TEXT 应用消息上行 中控按命令分发
    action.messages_ = std::move(result.messages_);
    // 完整 BINARY 分块上行 文件分块载荷含 20B 传输头 中控定位会话转发
    action.binaries_ = std::move(result.binary_messages_);
    // CLOSE 帧 回包并置关闭 写调度发完即回收
    if (result.close_) {
        action.close_ = true;
        action.responses_.push_back(WsFrame::build(WsOpcode::CLOSE, result.close_payload_));
    }
    return action;
}
