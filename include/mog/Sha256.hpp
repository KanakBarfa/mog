// Compact SHA-256 (FIPS 180-4) for determinism traces whose digests are
// verified across machines and by external tools.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace mog {

class Sha256 {
public:
    Sha256() noexcept { reset(); }

    void reset() noexcept {
        h_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        buffered_ = 0;
        total_bits_ = 0;
    }

    void update(const void* data, std::size_t len) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        total_bits_ += static_cast<std::uint64_t>(len) * 8;
        while (len > 0) {
            const std::size_t take = 64 - buffered_ < len ? 64 - buffered_ : len;
            std::memcpy(buf_ + buffered_, p, take);
            buffered_ += take;
            p += take;
            len -= take;
            if (buffered_ == 64) {
                compress(buf_);
                buffered_ = 0;
            }
        }
    }

    // 32-byte big-endian digest; the object must be reset() before reuse.
    void finish(unsigned char out[32]) noexcept {
        const std::uint64_t bits = total_bits_;
        const unsigned char pad = 0x80;
        update(&pad, 1);
        const unsigned char zero = 0;
        while (buffered_ != 56)
            update(&zero, 1);
        unsigned char tail[8];
        for (int i = 0; i < 8; ++i)
            tail[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        // Length block bypasses the bit counter on purpose.
        std::memcpy(buf_ + 56, tail, 8);
        compress(buf_);
        buffered_ = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            out[4 * i + 0] = static_cast<unsigned char>(h_[i] >> 24);
            out[4 * i + 1] = static_cast<unsigned char>(h_[i] >> 16);
            out[4 * i + 2] = static_cast<unsigned char>(h_[i] >> 8);
            out[4 * i + 3] = static_cast<unsigned char>(h_[i]);
        }
    }

    [[nodiscard]] std::array<unsigned char, 32> finish() noexcept {
        std::array<unsigned char, 32> out{};
        finish(out.data());
        return out;
    }

    [[nodiscard]] static std::string_view hex(const std::array<unsigned char, 32>& d,
                                              char* scratch) noexcept {
        static constexpr char kDigits[] = "0123456789abcdef";
        for (std::size_t i = 0; i < 32; ++i) {
            scratch[2 * i] = kDigits[d[i] >> 4];
            scratch[2 * i + 1] = kDigits[d[i] & 0xF];
        }
        return std::string_view(scratch, 64);
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) noexcept {
        return (x >> n) | (x << (32 - n));
    }

    void compress(const unsigned char block[64]) noexcept {
        static constexpr std::uint32_t kK[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) |
                   (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) |
                   static_cast<std::uint32_t>(block[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + s1 + ch + kK[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += hh;
    }

    std::array<std::uint32_t, 8> h_{};
    unsigned char buf_[64]{};
    std::size_t buffered_{};
    std::uint64_t total_bits_{};
};

} // namespace mog
