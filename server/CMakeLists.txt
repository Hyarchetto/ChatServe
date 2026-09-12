add_executable(chat_server main.cpp)
target_link_libraries(chat_server PRIVATE chat_common)
target_compile_options(chat_server PRIVATE -Wall -Wextra)
