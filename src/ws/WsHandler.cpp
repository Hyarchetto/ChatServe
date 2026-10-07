// WsHandler — WebSocket 协议决策器，纯函数实现
#include "ws/WsHandler.h"

#include <utility>

#include "ws/WsParser.h"
#include "ws/WsFrame.h"
#include "ws/WsOpcode.h"

WsAction WsHandler::handle(std::string_view buf, WsFragmentState& frag) const {
    WsAction action;
    while (true) {
        // 一帧一解析，从已消耗位置续
        WsResult r = WsParser::handle(buf.substr(action.consumed_), frag);
        action.consumed_ += r.consumed_;

        switch (r.type_) {
            // 凑不齐一帧，剩下的字节留给下次读事件
            case WsResultType::INCOMPLETE: {
                return action;
            }
            // 帧头即判出协议错误，回一条空 CLOSE 后断开
            case WsResultType::BAD_FRAME: {
                action.responses_.push_back(WsFrame::build(WsOpcode::CLOSE, {}));
                action.close_ = true;
                return action;
            }
            // 帧已吃完无产出，继续下一帧
            case WsResultType::CONSUMED: {
                break;
            }
            // 完整消息上行，中控按命令分发
            case WsResultType::MESSAGE: {
                (r.opcode_ == WsOpcode::BINARY ? action.binaries_ : action.messages_)
                    .push_back(std::move(r.payload_));
                break;
            }
            // PING 本地回 PONG
            case WsResultType::PING: {
                action.responses_.push_back(WsFrame::build(WsOpcode::PONG, r.payload_));
                break;
            }
            // CLOSE 帧，回包并置关闭，写调度发完即回收
            case WsResultType::CLOSE: {
                action.responses_.push_back(WsFrame::build(WsOpcode::CLOSE, r.payload_));
                action.close_ = true;
                break;
            }
        }
    }
}
