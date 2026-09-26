// 应用层命令路由 — 应用层心跳域
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/AppRouter.h"
#include "app/AppParser.h"
#include "ctrl/Session.h"

void AppRouter::register_heartbeat() {
    this->on("PING", [](std::shared_ptr<Session> sess, const AppMessage&) {
        std::vector<CtrlDown> results;
        results.push_back({std::move(sess), AppParser::build_frame("PONG", "")});
        return results;
    });
}
