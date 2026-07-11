// 迷你 SHA1 实现（RFC 3174），仅用于 WebSocket 握手
// 无外部依赖
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

class SHA1 {
public:
    SHA1() { reset(); }

    void update(const uint8_t* data, size_t len) {
        size_t idx = bit_count_ / 8 % 64;
        bit_count_ += len * 8;
        size_t free = 64 - idx;
        if (len >= free) {
            std::memcpy(buffer_ + idx, data, free);
            transform(buffer_);
            data += free;
            len -= free;
            while (len >= 64) {
                transform(data);
                data += 64;
                len -= 64;
            }
            idx = 0;
        }
        std::memcpy(buffer_ + idx, data, len);
    }

    void final(uint8_t digest[20]) {
        uint64_t bits = bit_count_;
        size_t idx = bit_count_ / 8 % 64;
        buffer_[idx++] = 0x80;

        if (idx > 56) {
            std::memset(buffer_ + idx, 0, 64 - idx);
            transform(buffer_);
            idx = 0;
        }

        std::memset(buffer_ + idx, 0, 56 - idx);
        for (int i = 7; i >= 0; --i) {
            buffer_[56 + (7 - i)] = (bits >> (i * 8)) & 0xFF;
        }
        transform(buffer_);

        for (int i = 0; i < 5; ++i) {
            digest[i * 4]     = (state_[i] >> 24) & 0xFF;
            digest[i * 4 + 1] = (state_[i] >> 16) & 0xFF;
            digest[i * 4 + 2] = (state_[i] >> 8) & 0xFF;
            digest[i * 4 + 3] = state_[i] & 0xFF;
        }
    }

    static void hash(const uint8_t* data, size_t len, uint8_t digest[20]) {
        SHA1 ctx;
        ctx.update(data, len);
        ctx.final(digest);
    }

    static std::string hash_to_string(const uint8_t digest[20]) {
        static const char hex[] = "0123456789abcdef";
        std::string s(40, '\0');
        for (int i = 0; i < 20; ++i) {
            s[i * 2]     = hex[digest[i] >> 4];
            s[i * 2 + 1] = hex[digest[i] & 0x0F];
        }
        return s;
    }

private:
    uint32_t state_[5];
    uint8_t  buffer_[64];
    uint64_t bit_count_;

    void reset() {
        state_[0] = 0x67452301;
        state_[1] = 0xEFCDAB89;
        state_[2] = 0x98BADCFE;
        state_[3] = 0x10325476;
        state_[4] = 0xC3D2E1F0;
        bit_count_ = 0;
    }

    static inline uint32_t rotl(uint32_t x, int n) {
        return (x << n) | (x >> (32 - n));
    }

    void transform(const uint8_t block[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24)
                 | (static_cast<uint32_t>(block[i * 4 + 1]) << 16)
                 | (static_cast<uint32_t>(block[i * 4 + 2]) << 8)
                 | block[i * 4 + 3];
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2],
                 d = state_[3], e = state_[4];

        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)       { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40)  { f = b ^ c ^ d;          k = 0x6ED9EBA1; }
            else if (i < 60)  { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;          k = 0xCA62C1D6; }

            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }

        state_[0] += a; state_[1] += b; state_[2] += c;
        state_[3] += d; state_[4] += e;
    }
};
