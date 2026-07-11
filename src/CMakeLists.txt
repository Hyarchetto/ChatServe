# chat_common 库 — 所有核心代码
add_library(chat_common STATIC
    http/HttpParser.cpp
    http/Router.cpp
    http/StaticFileServer.cpp
    http/ErrorResponse.cpp
    ws/WebSocketParser.cpp
    ws/WebSocketFrame.cpp
    ws/WebSocketUpgradeResponse.cpp
    ws/WebSocketAppParser.cpp
    ws/WebSocketRouter.cpp
    core/ThreadPool.cpp
    core/EventLoop.cpp
    chatroom/Room.cpp
    filetransfer/FileManager.cpp
    server/Reactor.cpp
    server/Acceptor.cpp
    server/ConnectionRouter.cpp
    server/MainReactor.cpp
)

target_include_directories(chat_common PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(chat_common PUBLIC pthread)
target_compile_options(chat_common PRIVATE -Wall -Wextra)
