// ConnHandler 用例 — 心跳扫描与升级握手的回包形态
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

// 一组装配好的 io 处理器，上行邮箱的回调本用例用不到故为空
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

// 造一对已连接 socket 并给测试端设收超时，判错时不会把整个用例挂住
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

    // 两个方向都超过硬超时，未升级的连接同样判死
    f.handler_.on_tick(std::chrono::steady_clock::now() + std::chrono::seconds(100));

    char buf[8];
    CHECK_EQ(recv(sv[1], buf, sizeof(buf), 0), ssize_t(0));  // 对端已关闭
    close(sv[1]);
}

TEST(conn_handler_stays_silent_on_idle_websocket) {
    Fixture f;
    int sv[2];
    make_pair(sv);
    f.handler_.add_connection(sv[0]);

    std::thread runner([&f]() { f.loop_.loop(); });

    // 走完握手连接才进入 ws 形态
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
    // 1xx 无 body，握手响应不该带 Content-Length
    CHECK(resp.find("content-length") == std::string::npos);

    // 注入未来的时刻驱动节拍，经 post 仍在 io 线程执行
    // 取六十秒未到硬超时，连接不该被判死，服务端也不该主动发任何帧
    f.loop_.post([&f]() {
        f.handler_.on_tick(std::chrono::steady_clock::now() + std::chrono::seconds(60));
    });

    // 收超时两秒，一个字节都收不到
    unsigned char buf[8] = {0};
    CHECK(recv(sv[1], buf, sizeof(buf), 0) < 0);

    // 越过硬超时，静默的连接在此收掉
    f.loop_.post([&f]() {
        f.handler_.on_tick(std::chrono::steady_clock::now() + std::chrono::seconds(100));
    });
    CHECK_EQ(recv(sv[1], buf, sizeof(buf), 0), ssize_t(0));  // 对端已关闭

    f.loop_.quit();
    runner.join();
    close(sv[1]);
}

TEST(conn_handler_announces_close_on_failed_upgrade) {
    Fixture f;
    int sv[2];
    make_pair(sv);
    f.handler_.add_connection(sv[0]);

    std::thread runner([&f]() { f.loop_.loop(); });

    // 缺 Sec-WebSocket-Key 的升级请求握手失败，回 400 后断开
    const std::string req =
        "GET /chat HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
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
    CHECK(resp.find("400") != std::string::npos);
    CHECK(resp.find("connection: close") != std::string::npos);

    f.loop_.quit();
    runner.join();
    close(sv[1]);
}

TEST(conn_handler_parses_request_split_across_reads) {
    Fixture f;
    int sv[2];
    make_pair(sv);
    f.handler_.add_connection(sv[0]);

    std::thread runner([&f]() { f.loop_.loop(); });

    // 头块远超单次 recv 缓冲，服务端要分多轮读入再拼起来解析
    // 打 / 这个内联页面，绕开静态目录，用例的工作目录里没有它
    const std::string req = "GET / HTTP/1.1\r\nHost: x\r\nX-Pad: " +
                            std::string(8 * 1024, 'a') + "\r\n\r\n";
    CHECK_EQ(send(sv[1], req.data(), req.size(), 0), ssize_t(req.size()));

    std::string resp;
    char tmp[1024];
    while (resp.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(sv[1], tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
        resp.append(tmp, static_cast<size_t>(n));
    }
    CHECK(resp.find("200") != std::string::npos);

    f.loop_.quit();
    runner.join();
    close(sv[1]);
}
