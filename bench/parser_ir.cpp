// Parser instruction meter: instructions retired per message under callgrind,
// collection toggled exactly around each parse pass over the corpus.

#include <mog/ITCHParser.hpp>

#include <support/CorpusGen.hpp>

#ifdef MOG_IR_METER
#include <valgrind/callgrind.h>
#include <valgrind/valgrind.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct FoldSink {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    void on_message(const mog::Message& m) noexcept { hash = mog::trace_hash(m, hash); }
};

} // namespace

int main(int argc, char** argv) {
    const std::size_t passes =
        argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : std::size_t{8};
    const auto stream =
        mog::testing::encode_corpus(mog::testing::make_corpus({0xBEEFULL, 1u << 20}));
    constexpr std::size_t kMsgs = 1u << 20;

    // Warm translations outside the collected region.
    FoldSink warm;
    if (!mog::parse_itch(stream.data(), stream.size(), warm)) {
        std::fprintf(stderr, "parser_ir FAIL: warmup parse\n");
        return 1;
    }

#ifdef MOG_IR_METER
    CALLGRIND_ZERO_STATS;
#endif
    [[maybe_unused]] std::uint64_t guard = 0;
    for (std::size_t p = 0; p < passes; ++p) {
        FoldSink sink;
#ifdef MOG_IR_METER
        CALLGRIND_TOGGLE_COLLECT;
#endif
        const auto consumed = mog::parse_itch(stream.data(), stream.size(), sink);
        const bool good = consumed.has_value() && *consumed == stream.size();
#ifdef MOG_IR_METER
        CALLGRIND_TOGGLE_COLLECT;
#endif
        if (!good) {
            std::fprintf(stderr, "parser_ir FAIL: parse pass %zu\n", p);
            return 1;
        }
        guard += sink.hash >> 32;
    }
#ifndef MOG_IR_METER
    std::printf("guard=%llu\n", static_cast<unsigned long long>(guard));
#else
    std::printf("messages=%llu\n", static_cast<unsigned long long>(passes * kMsgs));
#endif
    return 0;
}
