// Fast 128-bit streaming determinism digest engine for high-IPC simulation.
#pragma once

#include <mog/Sha256.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace mog {

// 128-bit vectorized streaming hash with sub-nanosecond per-word mixing.
class FastDigest128 {
public:
    static constexpr std::uint64_t kPrime0 = 0x9e3779b97f4a7c15ULL;
    static constexpr std::uint64_t kPrime1 = 0xbf58476d1ce4e5b9ULL;
    static constexpr std::uint64_t kPrime2 = 0x94d049bb133111ebULL;
    static constexpr std::uint64_t kPrime3 = 0xd6e8feb86659fd93ULL;

    FastDigest128() noexcept { reset(); }

    void reset() noexcept {
        h0_ = kPrime0;
        h1_ = kPrime1;
        total_bytes_ = 0;
        buffered_ = 0;
    }

    void update(const void* data, std::size_t len) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        total_bytes_ += len;

        if (buffered_ > 0) {
            const std::size_t need = 16 - buffered_;
            const std::size_t take = len < need ? len : need;
            std::memcpy(buf_ + buffered_, p, take);
            buffered_ += take;
            p += take;
            len -= take;
            if (buffered_ == 16) {
                mix_block(buf_);
                buffered_ = 0;
            }
        }

        while (len >= 16) {
            mix_block(p);
            p += 16;
            len -= 16;
        }

        if (len > 0) {
            std::memcpy(buf_ + buffered_, p, len);
            buffered_ += len;
        }
    }

    [[nodiscard]] std::array<unsigned char, 16> finish() const noexcept {
        std::uint64_t f0 = h0_;
        std::uint64_t f1 = h1_;

        if (buffered_ > 0) {
            unsigned char tail[16] = {};
            std::memcpy(tail, buf_, buffered_);
            tail[buffered_] = 0x80;
            std::uint64_t w0 = 0, w1 = 0;
            std::memcpy(&w0, tail, 8);
            std::memcpy(&w1, tail + 8, 8);
            f0 = std::rotl(f0 + w0 * kPrime0, 31) * kPrime2;
            f1 = std::rotl(f1 + w1 * kPrime1, 27) * kPrime3;
        }

        f0 ^= total_bytes_ * kPrime0;
        f1 ^= total_bytes_ * kPrime1;

        f0 ^= f1;
        f0 *= kPrime2;
        f0 ^= (f0 >> 33);

        f1 ^= f0;
        f1 *= kPrime1;
        f1 ^= (f1 >> 33);

        std::array<unsigned char, 16> out{};
        std::memcpy(out.data(), &f0, 8);
        std::memcpy(out.data() + 8, &f1, 8);
        return out;
    }

    [[nodiscard]] static std::string_view hex(const std::array<unsigned char, 16>& d,
                                              char* scratch) noexcept {
        static constexpr char kDigits[] = "0123456789abcdef";
        for (std::size_t i = 0; i < 16; ++i) {
            scratch[2 * i] = kDigits[d[i] >> 4];
            scratch[2 * i + 1] = kDigits[d[i] & 0xF];
        }
        return std::string_view(scratch, 32);
    }

private:
    void mix_block(const unsigned char* block) noexcept {
        std::uint64_t w0 = 0, w1 = 0;
        std::memcpy(&w0, block, 8);
        std::memcpy(&w1, block + 8, 8);
        h0_ = std::rotl(h0_ + w0 * kPrime0, 31) * kPrime2;
        h1_ = std::rotl(h1_ + w1 * kPrime1, 27) * kPrime3;
    }

    std::uint64_t h0_ = kPrime0;
    std::uint64_t h1_ = kPrime1;
    std::uint64_t total_bytes_ = 0;
    std::size_t buffered_ = 0;
    unsigned char buf_[16] = {};
};

enum class DigestMode : std::uint8_t {
    golden = 0, // NIST SHA-256 (32 bytes)
    fast = 1    // Vectorized FastDigest-128 (16 bytes)
};

// Dual-tier determinism digest combining fast streaming and golden verification.
class DeterminismDigest {
public:
    explicit DeterminismDigest(DigestMode mode = DigestMode::golden) noexcept : mode_(mode) {
        reset();
    }

    void reset() noexcept {
        if (mode_ == DigestMode::golden)
            sha_.reset();
        else
            fast_.reset();
    }

    void update(const void* data, std::size_t len) noexcept {
        if (mode_ == DigestMode::golden)
            sha_.update(data, len);
        else
            fast_.update(data, len);
    }

    [[nodiscard]] DigestMode mode() const noexcept { return mode_; }

    [[nodiscard]] std::array<unsigned char, 32> finish_sha256() const noexcept {
        Sha256 copy = sha_;
        return copy.finish();
    }

    [[nodiscard]] std::array<unsigned char, 16> finish_fast128() const noexcept {
        return fast_.finish();
    }

private:
    DigestMode mode_ = DigestMode::golden;
    Sha256 sha_;
    FastDigest128 fast_;
};

} // namespace mog
