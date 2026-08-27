// Golden-file test over the committed synthetic capture; embedded when #embed exists.

#include <mog/Build.hpp>
#include <mog/ITCHParser.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if __has_include(<mog/SampleEmbed.hpp>)
#include <mog/SampleEmbed.hpp>
#define MOG_SAMPLE_EMBEDDED 1
#endif

namespace {

constexpr std::uint64_t kExpectedStreamHash = 0x308e35b105cfe9d0ULL;

int fail(const char* what) noexcept {
    std::fprintf(stderr, "golden FAIL: %s\n", what);
    return 1;
}

struct Tally {
    std::size_t per_type[256] = {};
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    std::size_t count = 0;

    void on_message(const mog::Message& m) noexcept {
        per_type[static_cast<unsigned char>(m.type)]++;
        hash = mog::trace_hash(m, hash);
        ++count;
    }
};

[[nodiscard]] int check_stream(const unsigned char* data, std::size_t len) {
    Tally tally;
    auto consumed = mog::parse_itch(data, len, tally);
    if (!consumed || *consumed != len)
        return fail("sample did not consume cleanly");
    if (tally.count != 96)
        return fail("message count");
    constexpr std::size_t kExpectedPerType[7] = {14, 14, 14, 14, 14, 13, 13};
    constexpr char kTypes[7] = {'A', 'F', 'E', 'C', 'X', 'D', 'U'};
    for (int i = 0; i < 7; ++i) {
        if (tally.per_type[static_cast<unsigned char>(kTypes[i])] != kExpectedPerType[i]) {
            return fail("per-type count");
        }
    }
    if (tally.hash != kExpectedStreamHash) {
        std::fprintf(stderr, "golden: actual stream hash 0x%016llx\n",
                     static_cast<unsigned long long>(tally.hash));
        return fail("stream hash");
    }
    return 0;
}

#if !defined(MOG_SAMPLE_EMBEDDED)
[[nodiscard]] std::vector<unsigned char> read_file(const char* path) noexcept {
    std::vector<unsigned char> bytes;
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
        return bytes;
    unsigned char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        bytes.insert(bytes.end(), buf, buf + n);
    }
    std::fclose(f);
    return bytes;
}
#endif

} // namespace

int main() {
#if defined(MOG_SAMPLE_EMBEDDED)
    if (int r = check_stream(mog::testing::kSampleItch, mog::testing::kSampleItchSize); r != 0)
        return r;
#else
    const auto bytes = read_file(MOG_SAMPLE_PATH);
    if (bytes.empty())
        return fail("sample file missing");
    if (int r = check_stream(bytes.data(), bytes.size()); r != 0)
        return r;
#endif
    std::printf("golden OK\n");
    return 0;
}
