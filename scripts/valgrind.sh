#!/bin/bash
# valgrind 测试运行器
# 用法: ./scripts/valgrind.sh [--vg-args="额外valgrind参数"]
#
# 先编译再跑 valgrind，编译出错直接停
# 用 script 命令捕获输出，避免文件重定向的缓冲问题
# 日志输出到项目根目录的 valgrind.log

set -e

VG_ARGS=""
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG_FILE="$PROJECT_ROOT/valgrind.log"

for arg in "$@"; do
    case "$arg" in
        --vg-args=*)
            VG_ARGS="${arg#*=}"
            ;;
        *)
            echo "未知参数: $arg"
            echo "用法: $0 [--vg-args=\"额外valgrind参数\"]"
            exit 1
            ;;
    esac
done

cd "$PROJECT_ROOT"

echo "=== 编译 ==="
mkdir -p build
cd build
cmake .. 2>&1
make -j$(nproc) 2>&1
cd ..

echo ""
echo "=== valgrind ==="
echo "日志文件: $LOG_FILE"
echo "服务器已启动，按 Ctrl+C 停止"
echo ""

# script 创建伪终端，所有输出实时写到日志文件
script -q -c \
    "valgrind --leak-check=full -s $VG_ARGS ./build/server/chat_server" \
    -O "$LOG_FILE"
