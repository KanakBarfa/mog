// Writes deterministic synthetic ITCH captures for tests and demos.

#include <mog/Types.hpp>
#include <support/CorpusGen.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <string_view>
#include <vector>

namespace {

enum class GenError { bad_number, open_failed };

[[nodiscard]] std::expected<std::size_t, GenError> parse_count(std::string_view s) noexcept {
    if (s.empty() || s.size() > 12)
        return std::unexpected(GenError::bad_number);
    std::size_t value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9')
            return std::unexpected(GenError::bad_number);
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    return value;
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 0x5EEDULL;
    std::size_t count = 4096;
    std::int64_t price_center = 0;
    std::int64_t price_span = 10'000'000;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            auto n = parse_count(argv[++i]);
            if (!n) {
                std::fputs("bad --count\n", stderr);
                return 2;
            }
            count = *n;
        } else if (std::strcmp(argv[i], "--price-center") == 0 && i + 1 < argc) {
            price_center = std::strtoll(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--price-span") == 0 && i + 1 < argc) {
            price_span = std::strtoll(argv[++i], nullptr, 10);
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        std::fputs("usage: mog-gen-sample [--seed S] [--count N] "
                   "[--price-center C] [--price-span N] PATH|- \n",
                   stderr);
        return 2;
    }
    const auto messages = mog::testing::make_corpus({seed, count, price_center, price_span});
    const auto bytes = mog::testing::encode_corpus(messages);

    if (std::strcmp(path, "-") == 0) {
        if (std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size())
            return 1;
        return 0;
    }
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "gen-sample: cannot open %s\n", path);
        return 1;
    }
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (written != bytes.size())
        return 1;
    return 0;
}
