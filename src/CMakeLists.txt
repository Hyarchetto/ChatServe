# chat_common 库 — 所有核心代码
# 源文件按分层排列：core → chatroom → app → http → ws → conn → server
add_library(chat_common STATIC
    core/EventLoop.cpp
    core/ThreadPool.cpp

    chatroom/Room.cpp
    chatroom/TransferManager.cpp

    app/AppChatCommands.cpp
    app/AppParser.cpp
    app/AppRouter.cpp
    app/AppSignalCommands.cpp
    app/AppTransferCommands.cpp

    http/ErrorResponse.cpp
    http/HttpHandler.cpp
    http/HttpParser.cpp
    http/HttpRouter.cpp
    http/StaticFileServer.cpp

    ws/WsFrame.cpp
    ws/WsHandler.cpp
    ws/WsParser.cpp
    ws/WsUpgradeResponse.cpp

    conn/ConnHandler.cpp
    conn/WriteScheduler.cpp

    server/Acceptor.cpp
    server/CtrlDispatcher.cpp
    server/Gateway.cpp
    server/Reactor.cpp
    server/ReactorFactory.cpp
)

target_include_directories(chat_common PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(chat_common PUBLIC pthread)
target_compile_options(chat_common PRIVATE -Wall -Wextra)
