# HTTPS 透明 TCP 代理 — 同时支持 HTTP 和 WebSocket
# 用法: python3 scripts/https_proxy.py
# 启动后会打印客户端该怎么连

import os
import platform
import socket
import ssl
import subprocess
import threading

TARGET = ("127.0.0.1", 8080)
BIND = ("0.0.0.0", 8443)

IS_WSL = "microsoft" in platform.release().lower()

def detect_host():
    """客户端应使用的局域网地址
    WSL 下本机网卡是虚拟网段 局域网连不上 所以要绕开它去问 Windows 的网卡
    探测顺序 CHATSERVE_HOST 环境变量 → WSL 下问 Windows → 直接取本机网卡"""
    if os.environ.get("CHATSERVE_HOST"):
        return os.environ["CHATSERVE_HOST"]

    if IS_WSL:
        host = ""
        try:
            # Find-NetRoute 取走向外网那条路由的源地址 即真实局域网 IP 查询只读路由表不需要真的联网
            out = subprocess.run(
                ["powershell.exe", "-NoProfile", "-Command",
                 "(Find-NetRoute -RemoteIPAddress 1.1.1.1 | Select-Object -First 1).IPAddress"],
                capture_output=True, text=True, timeout=5)
            host = out.stdout.strip()
        except Exception:
            host = ""
    else:
        try:
            host = socket.gethostbyname(socket.gethostname())
        except OSError:
            host = ""

    if not host:
        print("警告: 探测不到局域网 IP，按 127.0.0.1 输出，可用 CHATSERVE_HOST 指定")
        return "127.0.0.1"
    return host

def handle_client(ssl_sock):
    """把 SSL 连接的原始字节流透传到 C++ 服务器"""
    raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        raw.connect(TARGET)
    except Exception:
        ssl_sock.close()
        return
    # 双向透传
    def forward(src, dst):
        try:
            while True:
                data = src.recv(65536)
                if not data:
                    break
                dst.sendall(data)
        except:
            pass
        finally:
            try:
                src.close()
            except:
                pass
            try:
                dst.close()
            except:
                pass
    t1 = threading.Thread(target=forward, args=(ssl_sock, raw), daemon=True)
    t2 = threading.Thread(target=forward, args=(raw, ssl_sock), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain("scripts/cert.pem", "scripts/key.pem")

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(BIND)
srv.listen(128)
print(f"HTTPS/TCP proxy on {BIND[0]}:{BIND[1]} -> {TARGET[0]}:{TARGET[1]}")
print("=== 客户端连接 ===")
print(f"本机开发:  npm start -- --server=https://localhost:{BIND[1]}")
print(f"局域网:    ChatServe.exe --server=https://{detect_host()}:{BIND[1]}")

try:
    while True:
        client, addr = srv.accept()
        try:
            ssl_client = ctx.wrap_socket(client, server_side=True)
            threading.Thread(target=handle_client, args=(ssl_client,), daemon=True).start()
        except:
            client.close()
except KeyboardInterrupt:
    print("\n[proxy] shutdown")
finally:
    srv.close()
