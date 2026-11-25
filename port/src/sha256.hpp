// Minimal self-contained SHA-256 implementation (zero-heap streaming + optional std::string hex).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef TWC98_NO_STL
#include <string>
#endif

namespace twc98 {

inline void sha256_bytes(const uint8_t* data, size_t len, uint8_t out_digest[32]) {
    static constexpr uint32_t kK[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    auto rotr = [](uint32_t x, uint32_t n) -> uint32_t {
        return (x >> n) | (x << (32u - n));
    };
    uint32_t h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    auto compress = [&](const uint8_t block[64]) {
        uint32_t w[64]{};
        for (size_t i = 0u; i < 16u; ++i) {
            const size_t p = i * 4u;
            w[i] = (static_cast<uint32_t>(block[p]) << 24) |
                   (static_cast<uint32_t>(block[p + 1u]) << 16) |
                   (static_cast<uint32_t>(block[p + 2u]) << 8) |
                   static_cast<uint32_t>(block[p + 3u]);
        }
        for (size_t i = 16u; i < 64u; ++i) {
            const uint32_t s0 = rotr(w[i - 15u], 7u) ^ rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
            const uint32_t s1 = rotr(w[i - 2u], 17u) ^ rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);
            w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (size_t i = 0u; i < 64u; ++i) {
            const uint32_t s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + s1 + ch + kK[i] + w[i];
            const uint32_t s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;
            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };

    size_t off = 0u;
    while (off + 64u <= len) {
        compress(data + off);
        off += 64u;
    }

    uint8_t tail[64]{};
    const size_t rem = len - off;
    if (rem > 0u) {
        std::memcpy(tail, data + off, rem);
    }
    tail[rem] = 0x80u;
    if (rem >= 56u) {
        compress(tail);
        std::memset(tail, 0, sizeof(tail));
    }
    const uint64_t bit_len = static_cast<uint64_t>(len) * 8ULL;
    for (int i = 0; i < 8; ++i) {
        tail[56 + i] = static_cast<uint8_t>((bit_len >> (56 - i * 8)) & 0xFFu);
    }
    compress(tail);

    for (size_t i = 0u; i < 8u; ++i) {
        out_digest[i * 4u + 0u] = static_cast<uint8_t>((h[i] >> 24) & 0xFFu);
        out_digest[i * 4u + 1u] = static_cast<uint8_t>((h[i] >> 16) & 0xFFu);
        out_digest[i * 4u + 2u] = static_cast<uint8_t>((h[i] >> 8) & 0xFFu);
        out_digest[i * 4u + 3u] = static_cast<uint8_t>(h[i] & 0xFFu);
    }
}

#ifndef TWC98_NO_STL
inline std::string sha256_hex(const uint8_t* data, size_t len) {
    uint8_t digest[32]{};
    sha256_bytes(data, len, digest);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64u, '0');
    for (size_t i = 0u; i < 32u; ++i) {
        out[i * 2u + 0u] = kHex[(digest[i] >> 4) & 0xFu];
        out[i * 2u + 1u] = kHex[digest[i] & 0xFu];
    }
    return out;
}
#endif

}  // namespace twc98
