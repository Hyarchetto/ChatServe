// 迷你 SHA1 实现，RFC 3174，仅用于 WebSocket 握手
// 全 constexpr，运行期与编译期共用同一份定义
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

class SHA1 {
private:
    // 五个 32 位链接变量，循环中整体前移
    // 前置声明供下面的常量与 transform 使用
    struct State {
        uint32_t h[5];
    };

public:
    using Digest = std::array<uint8_t, 20>;

    // 对 len 字节求摘要，运行期调用与常量求值走同一条路径
    static constexpr Digest hash(const uint8_t* data, size_t len) {
        State s = kInitState;
        size_t full = len / 64;
        for (size_t i = 0; i < full; ++i) {
            s = transform(s, data + i * 64);
        }

        // 收尾补 0x80 与 64 位大端比特长度，余量放不下长度时多占一块
        size_t rem = len - full * 64;
        uint8_t tail[128] = {};
        for (size_t i = 0; i < rem; ++i) {
            tail[i] = data[full * 64 + i];
        }
        tail[rem] = 0x80;
        size_t tail_len = rem + 9 <= 64 ? 64 : 128;
        uint64_t bits = static_cast<uint64_t>(len) * 8;
        for (int i = 0; i < 8; ++i) {
            tail[tail_len - 8 + i] = static_cast<uint8_t>(bits >> (56 - i * 8));
        }

        s = transform(s, tail);
        if (tail_len == 128) {
            s = transform(s, tail + 64);
        }

        Digest out{};
        for (int i = 0; i < 5; ++i) {
            out[i * 4]     = static_cast<uint8_t>(s.h[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(s.h[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(s.h[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(s.h[i]);
        }
        return out;
    }

    // 字符串字面量入口，N 含结尾的 '\0'
    template <size_t N>
    static constexpr Digest hash(const char (&text)[N]) {
        uint8_t bytes[N] = {};
        for (size_t i = 0; i + 1 < N; ++i) {
            bytes[i] = static_cast<uint8_t>(text[i]);
        }
        return hash(bytes, N - 1);
    }

private:
    static constexpr State kInitState{{
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u
    }};

    static constexpr uint32_t rotl(uint32_t x, int n) {
        return (x << n) | (x >> (32 - n));
    }

    // 消息扩展的第 I 字：前 16 字取自块内，其余由四个前值异或后循环左移一位
    // 结果直接写回 w 数组，I 是编译期常量，下标不必运行期计算
    template <int I>
    static constexpr void expand_one(const uint32_t (&w16)[16], uint32_t (&w)[80]) {
        if constexpr (I < 16) {
            w[I] = w16[I];
        } else {
            w[I] = rotl(w[I - 3] ^ w[I - 8] ^ w[I - 14] ^ w[I - 16], 1);
        }
    }

    // 折叠表达式按序填满 80 字，没有循环也没有轮号分支
    template <int... Is>
    static constexpr void expand(std::integer_sequence<int, Is...>,
                                 const uint32_t (&w16)[16], uint32_t (&w)[80]) {
        (expand_one<Is>(w16, w), ...);
    }

    // 轮函数，80 轮分四段各取一支
    template <int I>
    static constexpr uint32_t round_f(uint32_t b, uint32_t c, uint32_t d) {
        if constexpr (I < 20)      return (b & c) | (~b & d);
        else if constexpr (I < 40) return b ^ c ^ d;
        else if constexpr (I < 60) return (b & c) | (b & d) | (c & d);
        else                       return b ^ c ^ d;
    }

    // 轮常量，与轮函数同一分段
    template <int I>
    static constexpr uint32_t round_k() {
        if constexpr (I < 20)      return 0x5A827999u;
        else if constexpr (I < 40) return 0x6ED9EBA1u;
        else if constexpr (I < 60) return 0x8F1BBCDCu;
        else                       return 0xCA62C1D6u;
    }

    template <int I>
    static constexpr void round(State& s, const uint32_t (&w)[80]) {
        uint32_t t = rotl(s.h[0], 5) + round_f<I>(s.h[1], s.h[2], s.h[3])
                   + s.h[4] + round_k<I>() + w[I];
        s.h[4] = s.h[3];
        s.h[3] = s.h[2];
        s.h[2] = rotl(s.h[1], 30);
        s.h[1] = s.h[0];
        s.h[0] = t;
    }

    // 折叠表达式把 80 个轮号摊成一条顺序序列
    template <int... Is>
    static constexpr void rounds(std::integer_sequence<int, Is...>, State& s,
                                 const uint32_t (&w)[80]) {
        (round<Is>(s, w), ...);
    }

    static constexpr State transform(State s, const uint8_t* block) {
        State prev = s;
        uint32_t w16[16] = {};
        for (int i = 0; i < 16; ++i) {
            w16[i] = (static_cast<uint32_t>(block[i * 4]) << 24)
                   | (static_cast<uint32_t>(block[i * 4 + 1]) << 16)
                   | (static_cast<uint32_t>(block[i * 4 + 2]) << 8)
                   | static_cast<uint32_t>(block[i * 4 + 3]);
        }
        uint32_t w[80] = {};
        expand(std::make_integer_sequence<int, 80>{}, w16, w);
        // 80 轮直接改写 s，跑完 s 里是 a..e 而非新的链接变量，要把进入时的值加回去
        rounds(std::make_integer_sequence<int, 80>{}, s, w);
        for (int i = 0; i < 5; ++i) {
            s.h[i] += prev.h[i];
        }
        return s;
    }
};

// 编译期自检，向量取自 RFC 3174 与 RFC 6455
// 实现出错了这里编译不过，不必等跑起来才知道
namespace sha1_detail {

// 与十六进制文本逐字节比对，std::array 的比较在 C++17 不是 constexpr 只能手写
constexpr bool digest_matches(const SHA1::Digest& d, const char* hex) {
    for (size_t i = 0; i < 20; ++i) {
        uint8_t hi = static_cast<uint8_t>(hex[i * 2]);
        uint8_t lo = static_cast<uint8_t>(hex[i * 2 + 1]);
        hi = static_cast<uint8_t>(hi <= '9' ? hi - '0' : (hi | 0x20) - 'a' + 10);
        lo = static_cast<uint8_t>(lo <= '9' ? lo - '0' : (lo | 0x20) - 'a' + 10);
        if (d[i] != static_cast<uint8_t>(hi * 16 + lo)) {
            return false;
        }
    }
    return true;
}

}  // namespace sha1_detail

static_assert(sha1_detail::digest_matches(SHA1::hash(""), 
                                          "da39a3ee5e6b4b0d3255bfef95601890afd80709"),
              "SHA1 空串向量");
static_assert(sha1_detail::digest_matches(SHA1::hash("abc"), 
                                          "a9993e364706816aba3e25717850c26c9cd0d89d"),
              "SHA1 abc 向量");
// 56 字节正好卡在长度字段放不下的边界，收尾块要多占一块
static_assert(sha1_detail::digest_matches(SHA1::hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                                          "84983e441c3bd26ebaae4aa1f95129e5e54670f1"),
              "SHA1 56 字节边界向量");
// RFC 6455 §1.3 握手向量，也是本项目唯一实际用到的输入形状
static_assert(sha1_detail::digest_matches(SHA1::hash("dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11"),
                                          "b37a4f2cc0624f1690f64606cf385945b2bec4ea"),
              "SHA1 WebSocket 握手向量");
