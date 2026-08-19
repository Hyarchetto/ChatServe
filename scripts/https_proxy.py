# HTTPS 透明 TCP 代理 — 同时支持 HTTP 和 WebSocket
# 用法: python3 scripts/https_proxy.py
# 访问: https://192.168.1.24:8443/chat

import socket
import ssl
import threading

TARGET = ("172.26.72.3", 8080)
BIND = ("0.0.0.0", 8443)

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
print(f"手机访问: https://192.168.1.24:8443/chat")

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
