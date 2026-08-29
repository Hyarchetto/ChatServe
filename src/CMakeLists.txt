# chat_common 库 — 所有核心代码
add_library(chat_common STATIC
    core/ThreadPool.cpp
    core/EventLoop.cpp
    http/HttpParser.cpp
    http/HttpRouter.cpp
    http/StaticFileServer.cpp
    http/ErrorResponse.cpp
    http/HttpHandler.cpp
    ws/WsParser.cpp
    ws/WsFrame.cpp
    ws/WsUpgradeResponse.cpp
    ws/WsAppParser.cpp
    ws/WsAppRouter.cpp
    ws/WsHandler.cpp
    chatroom/Room.cpp
    chatroom/TransferManager.cpp
    conn/ConnHandler.cpp
    conn/WriteScheduler.cpp
    server/Reactor.cpp
    server/ReactorFactory.cpp
    server/Acceptor.cpp
)

target_include_directories(chat_common PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(chat_common PUBLIC pthread)
target_compile_options(chat_common PRIVATE -Wall -Wextra)
