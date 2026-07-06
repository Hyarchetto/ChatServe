#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"

# 构建类型：默认 Release，可传参覆盖
BUILD_TYPE="${1:-Release}"
if [ "$#" -gt 0 ]; then shift; fi

echo "=== Configuring with CMake (${BUILD_TYPE}) ==="
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$@"

echo ""
echo "=== Building (${BUILD_TYPE}) ==="
cmake --build "$BUILD" --parallel "$(nproc)"

echo ""
echo "=== Build complete ==="
echo "Type:   ${BUILD_TYPE}"
echo "Server: $BUILD/server/chat_server"
echo "Client: $BUILD/client/chat_client"
