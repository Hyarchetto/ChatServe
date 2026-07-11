// ChatServe 服务端入口
// ./build.sh
// ./build/server/chat_server

#include <iostream>
#include <csignal>
#include "../include/server/Reactor.h"

#define SERVER_PORT 8080

int main() {
    Reactor server;
    if (!server.init()) {
        std::cerr << "服务器初始化失败" << std::endl;
        return -1;
    }
    server.start_listen(SERVER_PORT);
    server.loop();
    return 0;
}
