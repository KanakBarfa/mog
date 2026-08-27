// ISA dispatch: detection returns a valid variant, all kernel tables agree, and the
// active table is one of them.

#include <mog/ITCHParser.hpp>
#include <mog/IsaDispatch.hpp>

#include <support/CorpusGen.hpp>

#include <cstdio>
#include <vector>

namespace {

int fail(const char* what) noexcept {
    std::fprintf(stderr, "isa FAIL: %s\n", what);
    return 1;
}

struct HashSink {
    std::vector<std::uint64_t>* out;
    void on_message(const mog::Message& m) noexcept { out->push_back(mog::trace_hash(m)); }
};

} // namespace

int main() {
    const auto messages = mog::testing::make_corpus({0x15AULL, 4096});
    const auto stream = mog::testing::encode_corpus(messages);

    std::vector<std::uint64_t> baseline;
    HashSink baseline_sink{&baseline};
    auto consumed = mog::parse_itch_as(stream.data(), stream.size(), baseline_sink,
                                       mog::kernels_for(mog::Isa::sse4_baseline));
    if (!consumed)
        return fail("baseline rejected corpus");

    for (const mog::Isa isa : {mog::Isa::avx2, mog::Isa::avx512}) {
        std::vector<std::uint64_t> variant;
        HashSink sink{&variant};
        auto c = mog::parse_itch_as(stream.data(), stream.size(), sink, mog::kernels_for(isa));
        if (!c || variant != baseline) {
            std::fprintf(stderr, "isa FAIL: divergence in %s\n", mog::isa_name(isa));
            return 1;
        }
    }

    const mog::Isa detected = mog::detect_isa();
    std::vector<std::uint64_t> active;
    HashSink active_sink{&active};
    auto c2 = mog::parse_itch(stream.data(), stream.size(), active_sink);
    if (!c2 || active != baseline)
        return fail("active kernel mismatch");

    std::printf("isa OK (detected %s)\n", mog::isa_name(detected));
    return 0;
}
