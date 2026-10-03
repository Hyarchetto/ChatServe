// AppHandler — 业务处理段，应用层命令在业务池线程上处理成待发帧
// 按值持 RoomManager 与 AppRouter，池引用注入，结果投回中控持有的结果邮箱
// 中控投一批 CtrlCmd 进来，逐条处理，结果经结果邮箱回中控
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../chatroom/Room.h"
#include "../core/Mailbox.h"
#include "../core/ThreadPool.h"
#include "../ctrl/CtrlMsg.h"
#include "../ctrl/Session.h"
#include "AppRouter.h"

class AppHandler {
public:
    using ResultBox = Mailbox<CtrlResult>;

    explicit AppHandler(ThreadPool& works);
    AppHandler(const AppHandler&) = delete;
    AppHandler& operator=(const AppHandler&) = delete;

    // 装配期注入中控的结果邮箱，须在任何 handle 之前完成
    void attach_result_box(ResultBox& box);

    // 中控投来一批命令，造闭包安排到业务池执行，返回没能投出去的那些会话
    // 池的投递全有或全无，返回空表示整批都投出去了
    std::vector<Session*> handle(std::vector<CtrlCmd> cmds);

private:
    // 池线程入口，按 kind 分派到文本路由、分块路由或收尾
    void handle_cmd(CtrlCmd cmd);
    // 解析一条文本命令并委托 AppRouter
    std::vector<CtrlDown> route(std::shared_ptr<Session> sess, const std::string& text);
    // 把一条二进制分块委托 AppRouter 转发给下载方
    std::vector<CtrlDown> route_chunk(std::shared_ptr<Session> sess, const std::string& data);

    ThreadPool& works_;                 // 注入，工厂持有，生命周期长于本对象
    ResultBox* result_box_ = nullptr;   // 中控持有，仅借用
    RoomManager room_mgr_;              // 必须排在 app_router_ 之前
    AppRouter app_router_;              // 构造时按引用绑定 room_mgr_
};
