// BenchTransfer — WebSocket 文件传输吞吐压测
//
// 每房间一对连接 上传方与下载方 与消息中继压测的收发分离同构
//   上传方 注册文件元数据后按 DWREQ 应答 BINARY 分块 分块字节现场生成 不读磁盘
//   下载方 收到 FILE 广播后发起 DOWNLOAD 每收一块回 DWACK 驱动滑动窗口前进
//
// 与消息中继的关键差别是传输自带流控 上传方收不到 DWREQ 就发不出下一块 服务端
// 在途数据有界 客户端不限速也灌不爆服务端 因此这里没有限速参数
//
// 中继吞吐 = 下载方实收分块正文总量 / 时长 上传方发出的字节数用于交叉验证
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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
// 与服务端 TransferManager::kChunkSize 一致 决定模板帧的载荷长度
constexpr size_t kChunkSize = 256 * 1024;
// BINARY 帧载荷前缀 [session_id:8][offset:8][data_size:4]
constexpr size_t kBinaryHeader = 20;
constexpr size_t kRecvBuf = 512 * 1024;
constexpr int kIoTimeoutSec = 10;
// 掩码固定 载荷恒定 整帧一次算好反复发
constexpr uint8_t kMaskKey[4] = {0x12, 0x34, 0x56, 0x78};

using Clock = std::chrono::steady_clock;

// 一个房间的一对连接
struct Pair {
    int up_fd_ = -1;
    int down_fd_ = -1;
    size_t filesize_ = 0;
    double seconds_ = 0.0;              // 该房间从发起下载到收满的耗时
};

// 全场共用的同步点与计数
struct Bench {
    std::atomic<int> joined_{0};        // 已完成入房应答的下载方
    std::atomic<int> broken_{0};        // 中途失败的连接数
    std::atomic<bool> go_{false};       // 主线程放行 各下载方同时发起传输
    std::atomic<size_t> sent_{0};       // 上传方发出的分块正文总量
    std::atomic<size_t> got_{0};        // 下载方实收的分块正文总量
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
std::string build_client_frame(WsOpcode opcode, const std::string& payload) {
    const size_t n = payload.size();
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));
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

// 一条服务端下行帧 服务端不加掩码 也不对应用消息做分片
struct ServerFrame {
    bool text_ = false;
    bool binary_ = false;
    std::string payload_;
};

// 从 buf 的 pos 处解一条服务端帧 数据不足一帧时返回 false 且不推进 pos
bool take_frame(const std::string& buf, size_t& pos, ServerFrame& out) {
    if (buf.size() - pos < 2) {
        return false;
    }
    const uint8_t b0 = static_cast<uint8_t>(buf[pos]);
    const uint8_t b1 = static_cast<uint8_t>(buf[pos + 1]);
    if ((b0 & 0x80) == 0 || (b1 & 0x80) != 0) {
        return false;  // 不接分片 也不接带掩码的下行帧
    }
    const uint8_t opcode = b0 & 0x0F;
    uint64_t len = b1 & 0x7F;
    size_t hdr = 2;
    if (len == 126) {
        if (buf.size() - pos < 4) {
            return false;
        }
        len = (static_cast<uint64_t>(static_cast<uint8_t>(buf[pos + 2])) << 8) |
              static_cast<uint8_t>(buf[pos + 3]);
        hdr = 4;
    }
    else if (len == 127) {
        if (buf.size() - pos < 10) {
            return false;
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | static_cast<uint8_t>(buf[pos + 2 + i]);
        }
        hdr = 10;
    }
    if (buf.size() - pos < hdr + static_cast<size_t>(len)) {
        return false;
    }
    out.payload_.assign(buf, pos + hdr, static_cast<size_t>(len));
    out.text_ = opcode == static_cast<uint8_t>(WsOpcode::TEXT);
    out.binary_ = opcode == static_cast<uint8_t>(WsOpcode::BINARY);
    pos += hdr + static_cast<size_t>(len);
    return true;
}

// 取竖线分隔协议帧里命令字之后的第 idx 个参数 不足返回空串
std::string param_at(const std::string& frame, size_t idx) {
    size_t start = frame.find('|');
    if (start == std::string::npos) {
        return {};
    }
    ++start;
    for (size_t i = 0; i < idx; ++i) {
        auto next = frame.find('|', start);
        if (next == std::string::npos) {
            return {};
        }
        start = next + 1;
    }
    auto end = frame.find('|', start);
    return frame.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// 把 20 字节分块头按固定掩码写进模板帧 载荷主体不变
// 掩码按位置异或 固定掩码下改写头部无需重算整个载荷
void patch_header(std::string& frame, size_t payload_off, uint64_t session_id,
                  uint64_t offset, uint32_t size) {
    uint8_t h[kBinaryHeader];
    std::memcpy(h, &session_id, 8);
    std::memcpy(h + 8, &offset, 8);
    std::memcpy(h + 16, &size, 4);
    for (size_t i = 0; i < kBinaryHeader; ++i) {
        frame[payload_off + i] = static_cast<char>(h[i] ^ kMaskKey[i % 4]);
    }
}

// 整条分块帧 分块大小不足一整块时只能现组
std::string build_chunk_frame(uint64_t session_id, uint64_t offset, size_t size) {
    std::string payload(kBinaryHeader + size, 'x');
    uint8_t h[kBinaryHeader];
    std::memcpy(h, &session_id, 8);
    std::memcpy(h + 8, &offset, 8);
    uint32_t sz = static_cast<uint32_t>(size);
    std::memcpy(h + 16, &sz, 4);
    std::memcpy(payload.data(), h, kBinaryHeader);
    return build_client_frame(WsOpcode::BINARY, payload);
}

// 上传方 注册文件后阻塞在 DWREQ 上 每来一条回一块
// 收不到 DWREQ 就发不出下一块 窗口与在途字节全由服务端与下载方控制
// tmpl 按值收 每个线程各持一份可改的模板帧 免去跨线程共享可变状态
void uploader_loop(Pair& pair, Bench& bench, std::string tmpl, size_t payload_off) {
    char buf[kRecvBuf];
    std::string acc;
    ServerFrame frame;
    while (true) {
        ssize_t n = ::recv(pair.up_fd_, buf, sizeof(buf), 0);
        if (n < 0 && errno == EAGAIN) {
            continue;  // 收包超时 传输收尾时对端不再请求 主线程随后关掉本连接
        }
        if (n <= 0) {
            break;
        }
        acc.append(buf, static_cast<size_t>(n));

        size_t pos = 0;
        while (take_frame(acc, pos, frame)) {
            if (!frame.text_ || frame.payload_.rfind("DWREQ|", 0) != 0) {
                continue;  // UPOK 与别家广播都不管
            }
            // DWREQ|session_id|file_id|offset|size
            const uint64_t session_id = std::strtoull(param_at(frame.payload_, 0).c_str(),
                                                      nullptr, 10);
            const uint64_t offset = std::strtoull(param_at(frame.payload_, 2).c_str(),
                                                  nullptr, 10);
            const size_t size = std::strtoul(param_at(frame.payload_, 3).c_str(),
                                             nullptr, 10);
            if (size == 0 || offset + size > pair.filesize_) {
                continue;
            }
            // 满块走模板 只改头 20 字节 末块长度不同只能现组
            if (size == kChunkSize) {
                patch_header(tmpl, payload_off, session_id, offset,
                             static_cast<uint32_t>(size));
                ::send(pair.up_fd_, tmpl.data(), tmpl.size(), 0);
            }
            else {
                const std::string tail = build_chunk_frame(session_id, offset, size);
                ::send(pair.up_fd_, tail.data(), tail.size(), 0);
            }
            bench.sent_ += size;
        }
        acc.erase(0, pos);
    }
    // 上传方收满后不再被请求 由主线程 shutdown 叫停 退出本身不代表失败
    // 它若中途出错 下载方就收不满 完成率会直接体现
}

// 下载方 等入房应答 等 FILE 广播 放行后发起下载并排空到收满
void downloader_loop(Pair& pair, Bench& bench) {
    char buf[kRecvBuf];
    std::string acc;
    ServerFrame frame;

    // 先等入房应答 主线程据此确认下载方已在房内 再让上传方注册文件
    bool joined = false;
    std::string file_id;
    while (!joined || file_id.empty()) {
        ssize_t n = ::recv(pair.down_fd_, buf, sizeof(buf), 0);
        if (n < 0 && errno == EAGAIN) {
            continue;
        }
        if (n <= 0) {
            bench.broken_ += 1;
            return;
        }
        acc.append(buf, static_cast<size_t>(n));

        size_t pos = 0;
        while (take_frame(acc, pos, frame)) {
            if (!frame.text_) {
                continue;
            }
            if (!joined && frame.payload_.rfind("OK|", 0) == 0) {
                joined = true;
                bench.joined_ += 1;
            }
            else if (frame.payload_.rfind("FILE|", 0) == 0) {
                file_id = param_at(frame.payload_, 0);
            }
        }
        acc.erase(0, pos);
    }

    while (!bench.go_.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    const std::string req = build_client_frame(WsOpcode::TEXT, "DOWNLOAD|" + file_id);
    ::send(pair.down_fd_, req.data(), req.size(), 0);

    size_t received = 0;
    bool clean = true;
    const auto t0 = Clock::now();
    while (received < pair.filesize_) {
        ssize_t n = ::recv(pair.down_fd_, buf, sizeof(buf), 0);
        if (n < 0 && errno == EAGAIN) {
            continue;
        }
        if (n <= 0) {
            clean = false;
            break;
        }
        acc.append(buf, static_cast<size_t>(n));

        size_t pos = 0;
        while (take_frame(acc, pos, frame)) {
            if (!frame.binary_) {
                // DWSTART DWDATA DWNDONE 都只用来驱动前端卡片 压测只认二进制块
                if (frame.text_ && frame.payload_.rfind("DWERR|", 0) == 0) {
                    clean = false;
                }
                continue;
            }
            if (frame.payload_.size() < kBinaryHeader) {
                continue;
            }
            // BINARY 载荷自带头 确认分块直接从头里取会话号与偏移
            uint64_t session_id = 0;
            uint64_t offset = 0;
            std::memcpy(&session_id, frame.payload_.data(), 8);
            std::memcpy(&offset, frame.payload_.data() + 8, 8);
            received += frame.payload_.size() - kBinaryHeader;

            const std::string ack = build_client_frame(
                WsOpcode::TEXT,
                "DWACK|" + std::to_string(session_id) + "|" + std::to_string(offset));
            ::send(pair.down_fd_, ack.data(), ack.size(), 0);
        }
        acc.erase(0, pos);
    }

    pair.seconds_ = std::chrono::duration<double>(Clock::now() - t0).count();
    bench.got_ += received;
    bench.broken_ += clean ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    // 服务端关连接后 send 会收到 SIGPIPE 默认动作是终止进程
    std::signal(SIGPIPE, SIG_IGN);

    const int rooms = (argc > 1) ? std::atoi(argv[1]) : 8;
    const size_t file_mb = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 64;
    const size_t filesize = file_mb * 1024 * 1024;

    std::printf("房间数 %d  每房间一上传方一下载方 共 %d 条连接\n", rooms, rooms * 2);
    std::printf("每房间文件 %zu MB  分块 %zu KB  窗口 8\n\n", file_mb, kChunkSize / 1024);

    // 建连入房 上传方与下载方同处一室
    // 先建满全部上传方再建全部下载方 服务端按 fd 分配 io 归属 成对建连会让
    // 相邻 fd 走上传方与下载方的交替规律 把整类角色压给同一条 io 从属
    std::vector<Pair> pairs(static_cast<size_t>(rooms));
    for (int i = 0; i < rooms; ++i) {
        Pair& pair = pairs[static_cast<size_t>(i)];
        pair.filesize_ = filesize;
        const std::string room = "tbench" + std::to_string(i);
        pair.up_fd_ = connect_ws();
        if (pair.up_fd_ >= 0) {
            const std::string join = build_client_frame(
                WsOpcode::TEXT, "JOIN|" + room + "|u" + std::to_string(i));
            ::send(pair.up_fd_, join.data(), join.size(), 0);
        }
    }
    for (int i = 0; i < rooms; ++i) {
        Pair& pair = pairs[static_cast<size_t>(i)];
        const std::string room = "tbench" + std::to_string(i);
        pair.down_fd_ = connect_ws();
        if (pair.down_fd_ >= 0) {
            const std::string join = build_client_frame(
                WsOpcode::TEXT, "JOIN|" + room + "|r" + std::to_string(i));
            ::send(pair.down_fd_, join.data(), join.size(), 0);
        }
    }
    int ok_pairs = 0;
    for (const Pair& pair : pairs) {
        if (pair.up_fd_ >= 0 && pair.down_fd_ >= 0) {
            ++ok_pairs;
        }
    }
    std::printf("建连成功 %d / %d 对\n", ok_pairs, rooms);
    if (ok_pairs == 0) {
        return 1;
    }

    Bench bench;
    std::vector<std::thread> dthreads;
    dthreads.reserve(pairs.size());
    for (size_t i = 0; i < pairs.size(); ++i) {
        dthreads.emplace_back(downloader_loop, std::ref(pairs[i]), std::ref(bench));
    }

    // 等下载方全部入房 此后注册文件 广播不会漏发
    const auto join_deadline = Clock::now() + std::chrono::seconds(10);
    while (bench.joined_.load() < ok_pairs && Clock::now() < join_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("入房就绪 %d / %d 对\n", bench.joined_.load(), ok_pairs);

    // 注册文件元数据 只报文件名与大小 分块等到 DWREQ 再发
    const std::string upload =
        build_client_frame(WsOpcode::TEXT,
                           "UPLOAD|bench.bin|" + std::to_string(filesize));
    for (Pair& pair : pairs) {
        if (pair.up_fd_ >= 0) {
            ::send(pair.up_fd_, upload.data(), upload.size(), 0);
        }
    }

    // 整条满块帧 载荷先填好 掩码固定 各分块只改前 20 字节
    std::string tmpl = build_client_frame(
        WsOpcode::BINARY, std::string(kBinaryHeader + kChunkSize, 'x'));
    const size_t payload_off = tmpl.size() - kBinaryHeader - kChunkSize;

    std::vector<std::thread> uthreads;
    uthreads.reserve(pairs.size());
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (pairs[i].up_fd_ >= 0) {
            uthreads.emplace_back(uploader_loop, std::ref(pairs[i]), std::ref(bench),
                                  tmpl, payload_off);
        }
    }

    const auto wall_start = Clock::now();
    bench.go_.store(true);
    for (auto& t : dthreads) {
        t.join();
    }
    const double wall = std::chrono::duration<double>(Clock::now() - wall_start).count();

    // 下载方收满后不再请求 上传方阻塞在 recv 上 关掉连接让它退出
    for (Pair& pair : pairs) {
        if (pair.up_fd_ >= 0) {
            ::shutdown(pair.up_fd_, SHUT_RDWR);
        }
    }
    for (auto& t : uthreads) {
        t.join();
    }
    for (Pair& pair : pairs) {
        if (pair.up_fd_ >= 0) {
            ::close(pair.up_fd_);
        }
        if (pair.down_fd_ >= 0) {
            ::close(pair.down_fd_);
        }
    }

    const size_t sent = bench.sent_.load();
    const size_t got = bench.got_.load();
    const size_t expect = filesize * static_cast<size_t>(ok_pairs);

    std::printf("中途失败连接 %d\n", bench.broken_.load());
    std::printf("期望正文 %zu MB  上传方发出 %zu MB  下载方实收 %zu MB  完成率 %.1f%%\n",
                expect / 1048576, sent / 1048576, got / 1048576,
                expect ? 100.0 * static_cast<double>(got) / static_cast<double>(expect)
                       : 0.0);
    std::printf("\n");
    std::printf("墙钟吞吐      %.2f MB/s   %d 房间并发  单房间 %.1f MB/s\n",
                static_cast<double>(got) / wall / 1048576.0, ok_pairs,
                static_cast<double>(got) / wall / 1048576.0 / ok_pairs);
    double slowest = 0.0;
    double fastest = 1e9;
    double total = 0.0;
    int counted = 0;
    for (const Pair& pair : pairs) {
        if (pair.seconds_ <= 0.0) {
            continue;
        }
        slowest = std::max(slowest, pair.seconds_);
        fastest = std::min(fastest, pair.seconds_);
        total += pair.seconds_;
        ++counted;
    }
    if (counted > 0) {
        std::printf("单房间耗时    最快 %.3fs  最慢 %.3fs  平均 %.3fs\n", fastest,
                    slowest, total / counted);
        std::printf("单房间吞吐    最快 %.0f MB/s  最慢 %.0f MB/s\n",
                    static_cast<double>(filesize) / fastest / 1048576.0,
                    static_cast<double>(filesize) / slowest / 1048576.0);
    }
    return 0;
}
