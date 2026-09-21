# 单元测试 — 手写极简断言 零第三方依赖
add_executable(chat_tests
    TestMain.cpp
    AppParserTest.cpp
    HttpHandlerTest.cpp
    HttpParserTest.cpp
    LazyBufferTest.cpp
    TransferManagerTest.cpp
    WsParserTest.cpp
)

target_link_libraries(chat_tests PRIVATE chat_common)
target_compile_options(chat_tests PRIVATE -Wall -Wextra)

add_test(NAME chat_tests COMMAND chat_tests)

# 压测客户端 — 不参与 ctest 需服务端运行时手动调用
# ./build/test/chat_bench [并发连接数] [持续秒数] [每个发送方条/s]
add_executable(chat_bench BenchClient.cpp)
target_link_libraries(chat_bench PRIVATE chat_common)
target_compile_options(chat_bench PRIVATE -Wall -Wextra)

# ./build/test/chat_transfer_bench [房间数] [每房间文件 MB]
add_executable(chat_transfer_bench BenchTransfer.cpp)
target_link_libraries(chat_transfer_bench PRIVATE chat_common)
target_compile_options(chat_transfer_bench PRIVATE -Wall -Wextra)
