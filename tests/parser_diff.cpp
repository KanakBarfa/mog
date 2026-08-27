// Differential test: Types.hpp reference codecs vs every ISA kernel over a
// deterministic corpus, plus truncation, unknown-type, and bad-side sweeps.
// Scale via MOG_DIFF_N (messages); the M1 gate run uses >= 100M.

#include <mog/ITCHParser.hpp>

#include <support/CorpusGen.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int fail(const char* what) noexcept {
    std::fprintf(stderr, "diff FAIL: %s\n", what);
    return 1;
}

struct HashSink {
    std::vector<std::uint64_t>* out;
    void on_message(const mog::Message& m) noexcept { out->push_back(mog::trace_hash(m)); }
};

[[nodiscard]] std::vector<std::uint64_t>
reference_hashes(const std::vector<unsigned char>& stream) noexcept {
    std::vector<std::uint64_t> hashes;
    std::size_t off = 0;
    while (off < stream.size()) {
        const std::uint8_t need = mog::kFrame.length[stream[off]];
        if (need == 0 || off + need > stream.size())
            return {};
        auto decoded = mog::decode_message(stream.data() + off, need);
        if (!decoded)
            return {};
        hashes.push_back(mog::trace_hash(*decoded));
        off += need;
    }
    return hashes;
}

[[nodiscard]] int check_kernels_vs_reference(const std::vector<unsigned char>& stream,
                                             const std::vector<std::uint64_t>& expected,
                                             mog::Isa isa) {
    std::vector<std::uint64_t> got;
    got.reserve(expected.size());
    HashSink sink{&got};
    auto consumed = mog::parse_itch_as(stream.data(), stream.size(), sink, mog::kernels_for(isa));
    if (!consumed || *consumed != stream.size())
        return fail("kernel rejected valid stream");
    if (got != expected)
        return fail("kernel/reference divergence");
    return 0;
}

[[nodiscard]] int truncation_sweep(const std::vector<mog::Message>& messages) {
    int idx = 0;
    for (const mog::Message& m : messages) {
        std::vector<unsigned char> bytes;
        mog::testing::append_encoded(bytes, m);
        const auto fast_rejects = [&](std::size_t cut) {
            mog::Message tmp{};
            return !mog::active_kernels().parse_one(bytes.data(), bytes.size() - cut, tmp);
        };
        const auto ref_result_at = [&](std::size_t cut) {
            return mog::decode_message(bytes.data(), bytes.size() - cut);
        };
        for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
            const bool ref_ok = ref_result_at(cut).has_value();
            if (ref_ok == fast_rejects(cut)) {
                std::fprintf(stderr, "first mismatch: msg#%d type=%c size=%zu cut=%zu\n", idx,
                             m.type, bytes.size(), cut);
                return fail("truncation accept/reject mismatch");
            }
        }
        if (fast_rejects(0) || !ref_result_at(0).has_value())
            return fail("full message must parse");
        ++idx;
    }
    return 0;
}

[[nodiscard]] int malformed_cases(std::vector<unsigned char>& proto) {
    // Unknown type byte with full header length available.
    proto[0] = 'Z';
    mog::Message tmp{};
    if (mog::active_kernels().parse_one(proto.data(), proto.size(), tmp))
        return fail("unknown type accepted");
    if (mog::decode_message(proto.data(), proto.size()).error() != mog::DecodeError::unknown_type)
        return fail("reference unknown-type mismatch");

    // Invalid side on an Add Order.
    proto[0] = 'A';
    proto[19] = 'Q';
    if (mog::active_kernels().parse_one(proto.data(), proto.size(), tmp))
        return fail("bad side accepted");
    if (mog::decode_message(proto.data(), proto.size()).error() != mog::DecodeError::invalid_side)
        return fail("reference bad-side mismatch");
    return 0;
}

} // namespace

int main() {
    std::size_t count = 200'000;
    if (const char* n = std::getenv("MOG_DIFF_N"); n != nullptr && *n != '\0') {
        count = static_cast<std::size_t>(std::strtoull(n, nullptr, 10));
    }

    const auto messages = mog::testing::make_corpus({0xD1FFULL, count});
    const auto stream = mog::testing::encode_corpus(messages);
    const auto expected = reference_hashes(stream);
    if (expected.size() != messages.size())
        return fail("reference walk failed");

    for (const mog::Isa isa : {mog::Isa::sse4_baseline, mog::Isa::avx2, mog::Isa::avx512}) {
        if (int r = check_kernels_vs_reference(stream, expected, isa); r != 0)
            return r;
    }

    if (int r = truncation_sweep(messages); r != 0)
        return r;

    std::vector<unsigned char> one =
        mog::testing::encode_corpus({mog::testing::make_corpus({7, 1}).at(0)});
    one.resize(mog::wire::kAddOrderSize); // first message of seed 7 is an A
    if (int r = malformed_cases(one); r != 0)
        return r;

    std::printf("diff OK (%zu messages x 3 kernels)\n", messages.size());
    return 0;
}
