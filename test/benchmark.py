"""
ChatServe 压测脚本
用法:
    python benchmark.py --http          # HTTP 吞吐测试
    python benchmark.py --ws            # WebSocket 并发测试
    python benchmark.py --all           # 全部测试
    python benchmark.py --http --path /app.js  # 测试大文件吞吐
    python benchmark.py --http --clients 50  # 指定 50 并发

可选参数:
    --host HOST     服务器地址 (默认 localhost)
    --port PORT     端口 (默认 8080)
    --path PATH     请求路径 (默认 /)
    --clients N     并发连接数 (默认 20)
    --requests N    每个连接请求数 (默认 100)
"""

import argparse
import hashlib
import base64
import os
import socket
import statistics
import struct
import sys
import threading
import time
from datetime import datetime

# =========================================
#  工具函数
# =========================================

RESET = "\033[0m"
GREEN = "\033[92m"
YELLOW = "\033[93m"
RED = "\033[91m"
CYAN = "\033[96m"
BOLD = "\033[1m"

def ok(msg):   print(f"  {GREEN}✓{RESET} {msg}")
def warn(msg): print(f"  {YELLOW}⚠{RESET} {msg}")
def fail(msg): print(f"  {RED}✗{RESET} {msg}")
def title(msg):
    print(f"\n{BOLD}{CYAN}{'='*60}{RESET}")
    print(f"{BOLD}{CYAN}  {msg}{RESET}")
    print(f"{BOLD}{CYAN}{'='*60}{RESET}\n")

# =========================================
#  HTTP 压测
# =========================================

def http_worker(host, port, path, results, idx, n_requests, timeout=5):
    """单个 HTTP 工作线程：连续发 GET 请求，记录耗时"""
    latencies = []
    total_bytes = 0
    errors = 0
    for _ in range(n_requests):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(timeout)
            sock.connect((host, port))

            req = (
                f"GET {path} HTTP/1.1\r\n"
                f"Host: {host}:{port}\r\n"
                f"Connection: keep-alive\r\n"
                f"\r\n"
            )
            start = time.perf_counter()
            sock.sendall(req.encode())

            # 读取完整响应：头部 + body
            resp = b""
            while b"\r\n\r\n" not in resp:
                try:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    resp += chunk
                except socket.timeout:
                    break

            # 解析 Content-Length，读完 body
            header_end = resp.find(b"\r\n\r\n")
            if header_end != -1:
                headers_raw = resp[:header_end]
                body = resp[header_end + 4:]
                cl = None
                for line in headers_raw.split(b"\r\n"):
                    if line.lower().startswith(b"content-length:"):
                        cl = int(line.split(b":")[1].strip())
                        break
                if cl is not None:
                    while len(body) < cl:
                        try:
                            chunk = sock.recv(4096)
                            if not chunk:
                                break
                            body += chunk
                        except socket.timeout:
                            break
                resp = resp[:header_end + 4] + body

            elapsed = time.perf_counter() - start
            total_bytes += len(resp)
            sock.close()

            # 检查响应是否包含 HTTP 状态码
            if b"HTTP/1.1 200" in resp or b"HTTP/1.1 404" in resp:
                latencies.append(elapsed)
            else:
                errors += 1
        except Exception:
            errors += 1
    results[idx] = (latencies, errors, total_bytes)


def bench_http(host, port, path, n_clients, n_requests):
    title(f"HTTP 压测 — {n_clients} 并发 × {n_requests} 请求/连接  (GET {path})")

    results = [None] * n_clients
    threads = []

    start = time.perf_counter()
    for i in range(n_clients):
        t = threading.Thread(target=http_worker, args=(host, port, path, results, i, n_requests))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()
    elapsed = time.perf_counter() - start

    # 汇总
    all_lat = []
    total_err = 0
    total_bytes = 0
    for lat_list, err_cnt, bytes_cnt in results:
        all_lat.extend(lat_list)
        total_err += err_cnt
        total_bytes += bytes_cnt

    total_ok = len(all_lat)
    total = total_ok + total_err

    print(f"  总请求:   {total}")
    print(f"  成功:     {total_ok}")
    print(f"  失败:     {total_err}")
    print(f"  总耗时:   {elapsed:.2f}s")
    if elapsed > 0:
        print(f"  RPS:      {total_ok / elapsed:.1f} req/s")
        mbps = (total_bytes / elapsed) / (1024 * 1024)
        print(f"  吞吐:     {mbps:.2f} MB/s")
        print(f"  平均响体: {total_bytes // total_ok} 字节/响应")
    if all_lat:
        print(f"  延迟 (ms):")
        print(f"    平均:   {statistics.mean(all_lat)*1000:.1f}")
        print(f"    中位数: {statistics.median(all_lat)*1000:.1f}")
        print(f"    最小:   {min(all_lat)*1000:.1f}")
        print(f"    最大:   {max(all_lat)*1000:.1f}")

    if total_err > 0:
        warn(f"  错误率:   {total_err/total*100:.1f}%")
    else:
        ok("  零错误")

    return total_ok, total_err, elapsed


# =========================================
#  WebSocket 并发测试
# =========================================

def _create_ws_key():
    """生成 WebSocket 握手所需的 Sec-WebSocket-Key"""
    raw = os.urandom(16)
    return base64.b64encode(raw).decode()

def _compute_accept(key):
    """计算服务器预期的 Sec-WebSocket-Accept"""
    GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
    sha1 = hashlib.sha1((key + GUID).encode()).digest()
    return base64.b64encode(sha1).decode()

def _ws_handshake(host, port, timeout=5):
    """完成 WebSocket 握手，返回 socket 对象"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.connect((host, port))

    key = _create_ws_key()
    req = (
        f"GET / HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"\r\n"
    )
    sock.sendall(req.encode())

    resp = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        resp += chunk
        if b"\r\n\r\n" in resp:
            break

    if b"101" not in resp:
        sock.close()
        return None

    # 验证 accept
    expected = _compute_accept(key)
    if expected.encode() not in resp:
        sock.close()
        return None

    return sock


def _send_ws_frame(sock, message):
    """发送一个掩码的 WebSocket TEXT 帧"""
    data = message.encode()
    length = len(data)
    header = bytearray()
    header.append(0x81)  # FIN + TEXT

    mask_key = os.urandom(4)

    if length < 126:
        header.append(length | 0x80)  # MASK 位
    elif length < 65536:
        header.append(0xFE)
        header.extend(struct.pack("!H", length))
    else:
        header.append(0xFF)
        header.extend(struct.pack("!Q", length))

    header.extend(mask_key)
    masked_data = bytes(b ^ mask_key[i % 4] for i, b in enumerate(data))
    sock.sendall(bytes(header) + masked_data)


def _recv_ws_frame(sock, timeout=5):
    """接收一个 WebSocket 帧（简单实现，不支持分片）"""
    sock.settimeout(timeout)
    try:
        first = sock.recv(2)
        if len(first) < 2:
            return None
        opcode = first[0] & 0x0F
        masked = (first[1] & 0x80) != 0
        length = first[1] & 0x7F

        if length == 126:
            raw = sock.recv(2)
            length = struct.unpack("!H", raw)[0]
        elif length == 127:
            raw = sock.recv(8)
            length = struct.unpack("!Q", raw)[0]

        mask_key = None
        if masked:
            mask_key = sock.recv(4)

        payload = b""
        while len(payload) < length:
            chunk = sock.recv(length - len(payload))
            if not chunk:
                break
            payload += chunk

        if mask_key:
            payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))

        return {"opcode": opcode, "data": payload}
    except socket.timeout:
        return None


def ws_worker(host, port, results, idx, timeout=5):
    """单个 WS 工作线程：握手 + 发一条消息 + 收回复"""
    try:
        sock = _ws_handshake(host, port, timeout)
        if sock is None:
            results[idx] = ("FAIL", "握手失败")
            return

        # 发送一条聊天消息
        _send_ws_frame(sock, "hello")

        # 等待回复
        frame = _recv_ws_frame(sock, timeout)
        sock.close()

        if frame is None:
            results[idx] = ("FAIL", "无回复")
        elif frame["opcode"] == 0x09:
            results[idx] = ("PONG", frame["data"])
        elif frame["opcode"] == 0x01:
            results[idx] = ("OK", frame["data"].decode(errors="replace"))
        else:
            results[idx] = ("OK", f"opcode={frame['opcode']}")
    except Exception as e:
        results[idx] = ("FAIL", str(e))


def bench_ws(host, port, n_clients):
    title(f"WebSocket 并发测试 — {n_clients} 个连接同时握手")

    results = [None] * n_clients
    threads = []

    start = time.perf_counter()
    for i in range(n_clients):
        t = threading.Thread(target=ws_worker, args=(host, port, results, i))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()
    elapsed = time.perf_counter() - start

    ok_count = 0
    pong_count = 0
    fail_count = 0
    fail_details = []

    for r in results:
        if r is None:
            fail_count += 1
            fail_details.append("无结果")
        elif r[0] == "OK":
            ok_count += 1
        elif r[0] == "PONG":
            pong_count += 1
        elif r[0] == "FAIL":
            fail_count += 1
            fail_details.append(r[1])

    print(f"  总连接:   {n_clients}")
    print(f"  成功:     {ok_count}")
    print(f"  PONG:     {pong_count}")
    print(f"  失败:     {fail_count}")
    print(f"  耗时:     {elapsed:.2f}s")

    if fail_count > 0 and len(fail_details) <= 5:
        for d in fail_details:
            warn(f"  {d}")
    elif fail_count > 0:
        warn(f"  前 5 个失败原因: {fail_details[:5]}")

    if fail_count == 0:
        ok("全部成功")
    elif fail_count > n_clients * 0.3:
        fail("失败率过高")

    return ok_count + pong_count, fail_count, elapsed


# =========================================
#  快速连接测试（检查服务器是否存活）
# =========================================

def ping_test(host, port):
    """快速检查服务器是否在监听"""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(2)
        sock.connect((host, port))
        sock.close()
        return True
    except Exception:
        return False


# =========================================
#  主入口
# =========================================

def main():
    parser = argparse.ArgumentParser(description="ChatServe 压测脚本")
    parser.add_argument("--host", default="localhost", help="服务器地址")
    parser.add_argument("--port", type=int, default=8080, help="端口")
    parser.add_argument("--path", default="/", help="请求路径 (默认 /)")
    parser.add_argument("--clients", type=int, default=20, help="并发连接数")
    parser.add_argument("--requests", type=int, default=100, help="每个连接请求数")
    parser.add_argument("--http", action="store_true", help="HTTP 压测")
    parser.add_argument("--ws", action="store_true", help="WebSocket 压测")
    parser.add_argument("--all", action="store_true", help="全部测试")
    args = parser.parse_args()

    print(f"\n{CYAN}ChatServe 压测工具{RESET}")
    print(f"目标: {args.host}:{args.port}")
    print(f"时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")

    # 存活检测
    if not ping_test(args.host, args.port):
        fail(f"服务器 {args.host}:{args.port} 未响应，请先启动服务")
        sys.exit(1)
    ok("服务器在线\n")

    run_http = args.http or args.all
    run_ws = args.ws or args.all

    if not run_http and not run_ws:
        warn("未指定测试类型，默认执行全部")
        run_http = True
        run_ws = True

    if run_http:
        bench_http(args.host, args.port, args.path, args.clients, args.requests)

    if run_ws:
        bench_ws(args.host, args.port, args.clients)

    print()


if __name__ == "__main__":
    main()
