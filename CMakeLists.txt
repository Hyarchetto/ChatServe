cmake_minimum_required(VERSION 3.14)
project(ChatServe VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# VS Code 智能提示，clangd / IntelliSense
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(src)
add_subdirectory(server)
add_subdirectory(test)
