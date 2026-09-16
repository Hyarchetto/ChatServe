# ChatServe 一键启动
# ./scripts/run.sh
cd "$(dirname "$0")/.."

if ! ./build.sh; then
    echo "构建失败，未启动任何服务"
    exit 1
fi

echo ""
echo "=== 启动 HTTPS 代理 (8443 -> 8080) ==="
if command -v python3 >/dev/null 2>&1 && [ -f scripts/cert.pem ] && [ -f scripts/key.pem ]; then
    python3 -u scripts/https_proxy.py &
    PROXY_PID=$!
    # 稍候检查存活，捕获端口占用等启动即退出的情况
    sleep 0.3
    PROXY_STAT="$(ps -p "$PROXY_PID" -o stat= 2>/dev/null)"
    if [ -n "$PROXY_STAT" ] && [ "$PROXY_STAT" != "Z" ]; then
        echo "HTTPS 代理已后台启动 (PID $PROXY_PID)"
        trap 'kill "$PROXY_PID" 2>/dev/null; wait "$PROXY_PID" 2>/dev/null' EXIT
    else
        echo "警告: HTTPS 代理启动失败，请检查 8443 端口是否被占用"
        wait "$PROXY_PID" 2>/dev/null
    fi
else
    echo "警告: 缺少 python3 或 scripts/cert.pem / scripts/key.pem，跳过 HTTPS 代理"
fi

echo ""
echo "=== 启动服务器 ==="
./build/server/chat_server
