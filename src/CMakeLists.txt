# chat_common 库 — 所有核心代码
add_library(chat_common STATIC
    http/HttpParser.cpp
    http/HttpRouter.cpp
    http/StaticFileServer.cpp
    http/ErrorResponse.cpp
    ws/WsParser.cpp
    ws/WsFrame.cpp
    ws/WsUpgradeResponse.cpp
    ws/WsAppParser.cpp
    ws/WsAppRouter.cpp
    core/ThreadPool.cpp
    core/EventLoop.cpp
    chatroom/Room.cpp
    transfer/TransferManager.cpp
    server/Reactor.cpp
    server/WriteScheduler.cpp
    server/Acceptor.cpp
    server/HttpHandler.cpp
    server/WsHandler.cpp
)

target_include_directories(chat_common PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(chat_common PUBLIC pthread)
target_compile_options(chat_common PRIVATE -Wall -Wextra)
