// WebSocket 应用层消息路由 — 解析后消息 → 处理器
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "WebSocketAppMessage.h"
#include "../server/Connection.h"
#include "../chatroom/Room.h"

class FileManager;  // 前向声明

// 待发送的定向消息（目标连接 + 已组帧数据）
struct WebSocketTargetedMessage {
    std::shared_ptr<Connection> target_;
    std::string data_;                      // 已封装为 WebSocket 帧的数据
};

class WebSocketRouter {
public:
    WebSocketRouter();

    // 处理器：接收解析后的消息、发起连接、房间管理器、文件管理器，返回待发送消息列表
    using Handler = std::function<std::vector<WebSocketTargetedMessage>(
        const WebSocketAppMessage& msg,
        const std::shared_ptr<Connection>& conn,
        RoomManager& room_mgr,
        FileManager& file_mgr)>;

    // 注册命令处理器
    void on(const std::string& command, Handler handler);

    // 注册默认处理器（裸文本/未识别的命令）
    void on_default(Handler handler);

    // 路由消息，结果写入 out；返回是否匹配到了处理器
    bool route(const WebSocketAppMessage& msg,
               const std::shared_ptr<Connection>& conn,
               RoomManager& room_mgr,
               FileManager& file_mgr,
               std::vector<WebSocketTargetedMessage>& out) const;

private:
    std::unordered_map<std::string, Handler> handlers_;
    Handler default_handler_;
};
