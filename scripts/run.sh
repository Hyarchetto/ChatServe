# ChatServe 一键启动
# ./scripts/run.sh
cd "$(dirname "$0")/.."
./build.sh && echo "" && echo "=== 启动服务器 ===" && ./build/server/chat_server
