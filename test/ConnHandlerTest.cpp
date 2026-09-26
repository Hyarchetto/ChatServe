// ConnHandler 用例 — 心跳扫描的两条分支
#include "TestMain.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "conn/ConnHandler.h"

namespace {

// 一组装配好的 io 处理器 上行邮箱的回调本用例用不到故为空
// 两个邮箱都照 Reactor 的装配顺序 attach 顺带覆盖唤醒 fd 的注册
struct Fixture {
    EventLoop loop_;
    Mailbox<CtrlUp> uplink_;
    ConnHandler handler_;

    Fixture()
        : uplink_([](std::vector<CtrlUp>&) {}),
          handler_(loop_, 0, uplink_) {
        CHECK(this->loop_.init());
        CHECK(this->uplink_.attach(this->loop_));
        CHECK(this->handler_.downlink_box().attach(this->loop_));
    }
};

// 造一对已连接 socket 并给测试端设收超时 判错时不会把整个用例挂住
void make_pair(int sv[2]) {
    CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    timeval tv{};
    tv.tv_sec = 2;
    setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

}  // namespace

TEST(conn_handler_closes_connection_idle_in_both_directions) {
    Fixture f;
    int sv[2];
    make_pair(sv);
    f.handler_.add_connection(sv[0]);

    // 两个方向都超过硬超时 未升级的连接同样判死
    f.handler_.on_tick(std::chrono::steady_clock::now() + std::chrono::seconds(100));

    char buf[8];
    CHECK_EQ(recv(sv[1], buf, sizeof(buf), 0), ssize_t(0));  // 对端已关闭
    close(sv[1]);
}

TEST(conn_handler_pings_idle_websocket) {
    Fixture f;
    int sv[2];
    make_pair(sv);
    f.handler_.add_connection(sv[0]);

    std::thread runner([&f]() { f.loop_.loop(); });

    // 走完握手连接才进入 ws 形态 未升级的连接不发 PING
    const std::string req =
        "GET /chat HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    CHECK_EQ(send(sv[1], req.data(), req.size(), 0), ssize_t(req.size()));

    std::string resp;
    char tmp[512];
    while (resp.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(sv[1], tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
        resp.append(tmp, static_cast<size_t>(n));
    }
    CHECK(resp.find("101") != std::string::npos);

    // 注入未来的时刻驱动节拍 经 post 仍在 io 线程执行
    // 取六十秒 入站静默过一个节拍该发 PING 出站还在硬超时之内不该判死
    f.loop_.post([&f]() {
        f.handler_.on_tick(std::chrono::steady_clock::now() + std::chrono::seconds(60));
    });

    // 一条空的 PING 帧 服务端出帧不带掩码 载荷长度为 0
    unsigned char ping[8] = {0};
    ssize_t n = recv(sv[1], ping, sizeof(ping), 0);
    CHECK_EQ(n, ssize_t(2));
    CHECK_EQ(ping[0], static_cast<unsigned char>(0x89));
    CHECK_EQ(ping[1], static_cast<unsigned char>(0x00));

    // 探测帧自己写出去了不算出站推进 否则连接每轮都被自己续命 两个方向都静默也判不死
    f.loop_.post([&f]() {
        f.handler_.on_tick(std::chrono::steady_clock::now() + std::chrono::seconds(100));
    });
    CHECK_EQ(recv(sv[1], ping, sizeof(ping), 0), ssize_t(0));  // 对端已关闭

    f.loop_.quit();
    runner.join();
    close(sv[1]);
}
