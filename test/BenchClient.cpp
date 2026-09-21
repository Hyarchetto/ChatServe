// BenchClient — WebSocket 消息中继吞吐压测
//
// 连接分为两类 每类各占一半 两两配对同处一室
//   发送方 只发 MSG 不收 下行只有一次性的 JOIN 应答 不会积压
//   接收方 只收不发 始终在读 也不会积压
// 收发分离是这份压测的关键 若让一条连接既发又收 发送速度一旦超过
// 服务端的下行速度 该连接就会被服务端的慢客户端保护断开 量到的
// 只是客户端的收发循环上限 不是服务端的中继能力
//
// 中继吞吐 = 发送方发出的消息正文总量 / 时长 即服务端接收 解析 分发并
// 转发出去的业务消息量 接收方收到的字节数用于交叉验证中继完整
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ws/WsFrame.h"
#include "ws/WsOpcode.h"

namespace {

constexpr int kPort = 8080;
constexpr const char* kHost = "127.0.0.1";
constexpr char kMsgPrefix[] = "MSG|";            // 命令前缀计入正文长度
constexpr size_t kMsgPrefixLen = 4;
constexpr size_t kBodyLen = 200;                 // 消息内容长度
constexpr size_t kMsgLen = kMsgPrefixLen + kBodyLen;
constexpr size_t kRecvBuf = 256 * 1024;
constexpr int kIoTimeoutSec = 5;                 // 收发超时 兼作断连探测
// 掩码固定 载荷恒定 整帧一次算好反复发
// 服务端解掩码的开销与随机掩码完全相同 随机化只会拖慢客户端
constexpr uint8_t kMaskKey[4] = {0x12, 0x34, 0x56, 0x78};

// 一类连接的结果
struct SideStat {
    std::atomic<size_t> bytes_{0};
    std::atomic<size_t> conns_{0};
    std::atomic<size_t> clean_{0};               // 跑满全程的条数
    std::atomic<int> first_errno_{0};            // 首个非正常结束的 errno
    std::atomic<const char*> first_what_{nullptr};
};

// 设收发超时 对端长时间无动静时让阻塞调用返回 失败返回 false
bool set_timeouts(int fd) {
    timeval tv{};
    tv.tv_sec = kIoTimeoutSec;
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

// 建连并完成 WebSocket 握手 成功返回 fd 失败返回 -1
int connect_ws() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    ::inet_pton(AF_INET, kHost, &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    // 客户端不校验 Sec-WebSocket-Accept 握手键固定即可
    const std::string req =
        "GET /chat HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (::send(fd, req.data(), req.size(), 0) < 0) {
        ::close(fd);
        return -1;
    }
    char buf[4096];
    std::string head;
    while (head.find("\r\n\r\n") == std::string::npos && head.size() <= 8192) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            ::close(fd);
            return -1;
        }
        head.append(buf, static_cast<size_t>(n));
    }
    // 状态行前缀比对 不能用 compare(0, 首行长度, ...) 长度不等必然失败
    if (head.rfind("HTTP/1.1 101", 0) != 0 || !set_timeouts(fd)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// 组一条客户端帧 MASK 位置 1 载荷按固定掩码异或
std::string build_client_frame(const std::string& payload) {
    const size_t n = payload.size();
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(WsOpcode::TEXT)));
    if (n < 126) {
        frame.push_back(static_cast<char>(0x80 | n));
    }
    else if (n <= 0xFFFF) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((n >> 8) & 0xFF));
        frame.push_back(static_cast<char>(n & 0xFF));
    }
    else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(
                static_cast<char>((static_cast<uint64_t>(n) >> (i * 8)) & 0xFF));
        }
    }
    frame.append(reinterpret_cast<const char*>(kMaskKey), sizeof(kMaskKey));
    std::string body = payload;
    WsFrame::apply_mask(reinterpret_cast<uint8_t*>(body.data()), body.size(), kMaskKey);
    frame += body;
    return frame;
}

// 记一次非正常结束 只保留第一条
void note_failure(SideStat& stat, const char* what) {
    int expected = 0;
    if (stat.first_errno_.compare_exchange_strong(expected, errno ? errno : -1)) {
        stat.first_what_.store(what);
    }
}

// 发送方 只发不收
// rate 为 0 时不节流 服务端吸收多快就转多快 会把服务器灌到内存耗尽
// rate 大于 0 时按目标速率节流 用于测服务端在可持续负载下的中继能力
void sender_loop(int fd, const std::string& frame,
                 std::chrono::steady_clock::time_point deadline, SideStat& stat,
                 int rate) {
    size_t total = 0;
    size_t sent = 0;
    bool clean = true;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        if (rate > 0) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                    .count();
            // 加一让首条立即发出 之后按配额走
            if (sent >= static_cast<size_t>(elapsed * rate) + 1) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
        }
        ssize_t n = ::send(fd, frame.data(), frame.size(), 0);
        if (n > 0) {
            total += static_cast<size_t>(n);
            ++sent;
            continue;
        }
        if (n < 0 && errno == EAGAIN) {
            continue;  // 发送超时 服务端一时跟不上 继续推
        }
        note_failure(stat, n == 0 ? "send 返回 0" : "send 失败");
        clean = false;
        break;
    }
    stat.bytes_ += total;
    stat.clean_ += clean ? 1 : 0;
    ::close(fd);
}

// 接收方 只收不发 始终排空 保证服务端下行不积压
void receiver_loop(int fd, std::chrono::steady_clock::time_point deadline,
                   SideStat& stat) {
    char buf[kRecvBuf];
    size_t total = 0;
    bool clean = true;
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            total += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EAGAIN) {
            continue;  // 收包超时 对端一时没消息 继续等
        }
        note_failure(stat, n == 0 ? "对端关闭" : "recv 失败");
        clean = false;
        break;
    }
    stat.bytes_ += total;
    stat.clean_ += clean ? 1 : 0;
    ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
    // 服务端关连接后 send 会收到 SIGPIPE 默认动作是终止进程
    std::signal(SIGPIPE, SIG_IGN);

    const int pairs = (argc > 1) ? std::atoi(argv[1]) : 50;   // 每对含一发一收
    const int seconds = (argc > 2) ? std::atoi(argv[2]) : 10;
    const int rate = (argc > 3) ? std::atoi(argv[3]) : 0;     // 每个发送方的目标条/s

    const std::string frame =
        build_client_frame(std::string(kMsgPrefix) + std::string(kBodyLen, 'x'));
    std::printf("连接对 %d  共 %d 条连接（一半发送方 一半接收方） 持续 %ds\n",
                pairs, pairs * 2, seconds);
    std::printf("单条 MSG 正文 %zuB  客户端帧 %zuB\n", kMsgLen, frame.size());
    if (rate > 0) {
        std::printf("发送方限速 每个 %d 条/s  合计目标 %d 条/s\n\n", rate, rate * pairs);
    }
    else {
        std::printf("发送方不限速 服务端吸收多快就转多快\n\n");
    }

    // 先全部建连并入房 不计入计时
    std::vector<int> senders;
    std::vector<int> receivers;
    for (int i = 0; i < pairs; ++i) {
        const std::string room = "bench" + std::to_string(i);
        int a = connect_ws();
        if (a >= 0) {
            const std::string join = build_client_frame("JOIN|" + room + "|s" +
                                                        std::to_string(i));
            ::send(a, join.data(), join.size(), 0);
            senders.push_back(a);
        }
        int b = connect_ws();
        if (b >= 0) {
            const std::string join = build_client_frame("JOIN|" + room + "|r" +
                                                        std::to_string(i));
            ::send(b, join.data(), join.size(), 0);
            receivers.push_back(b);
        }
    }
    std::printf("建连成功 发送方 %zu  接收方 %zu\n", senders.size(), receivers.size());
    if (senders.empty() || receivers.empty()) {
        return 1;
    }

    SideStat send_stat;
    SideStat recv_stat;
    std::vector<std::thread> threads;
    threads.reserve(senders.size() + receivers.size());
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(seconds);
    for (int fd : senders) {
        threads.emplace_back(
            [&, fd]() { sender_loop(fd, frame, deadline, send_stat, rate); });
    }
    for (int fd : receivers) {
        threads.emplace_back([&, fd]() { receiver_loop(fd, deadline, recv_stat); });
    }
    for (auto& t : threads) {
        t.join();
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    const size_t sent = send_stat.bytes_.load();
    const size_t got = recv_stat.bytes_.load();
    // 发送侧只说明服务端吞下了多少 真正被中继出去的要看接收侧
    const size_t sent_frames = sent / frame.size();
    // 服务端帧无掩码 头为 1+1+2 字节
    const size_t kServerFrame = kMsgLen + 4;
    const size_t relayed_frames = got / kServerFrame;
    const size_t relayed = relayed_frames * kMsgLen;

    std::printf("实际时长      %.2fs\n", elapsed);
    std::printf("发送方跑满    %zu / %zu\n", send_stat.clean_.load(), senders.size());
    std::printf("接收方跑满    %zu / %zu\n", recv_stat.clean_.load(), receivers.size());
    if (send_stat.first_errno_.load() != 0) {
        std::printf("发送方首个异常 errno=%d %s  %s\n", send_stat.first_errno_.load(),
                    std::strerror(send_stat.first_errno_.load()),
                    send_stat.first_what_.load());
    }
    if (recv_stat.first_errno_.load() != 0) {
        std::printf("接收方首个异常 errno=%d %s  %s\n", recv_stat.first_errno_.load(),
                    std::strerror(recv_stat.first_errno_.load()),
                    recv_stat.first_what_.load());
    }
    std::printf("发送侧吞入    %zu 条  接收侧中继 %zu 条  中继完成率 %.1f%%\n",
                sent_frames, relayed_frames,
                sent_frames ? 100.0 * static_cast<double>(relayed_frames) /
                              static_cast<double>(sent_frames) : 0.0);
    std::printf("线路字节      发送 %.1f MB  接收 %.1f MB\n",
                static_cast<double>(sent) / 1048576.0,
                static_cast<double>(got) / 1048576.0);
    std::printf("\n");
    std::printf("中继吞吐      %.2f MB/s  %.0f 条/s  平均单条 %zuB\n",
                static_cast<double>(relayed) / elapsed / 1048576.0,
                static_cast<double>(relayed_frames) / elapsed, kMsgLen);
    std::printf("吞入吞吐      %.2f MB/s  %.0f 条/s  仅发送侧 不代表已中继出去\n",
                static_cast<double>(sent) / elapsed / 1048576.0,
                static_cast<double>(sent_frames) / elapsed);
    return 0;
}
