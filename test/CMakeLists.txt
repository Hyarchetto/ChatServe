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
