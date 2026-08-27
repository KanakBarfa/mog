// Parser throughput benchmarks plus the hand-written asm twin attempt for the
// Add Order decode kernel (doctrine #4 in DESIGN.md: keep only on a >= 15% win).

#include <mog/ITCHParser.hpp>

#include <support/CorpusGen.hpp>

#include <benchmark/benchmark.h>
#include <cstdint>
#include <vector>

namespace {

const std::vector<unsigned char>& corpus_stream() {
    static const std::vector<unsigned char> stream =
        mog::testing::encode_corpus(mog::testing::make_corpus({0xBEEFULL, 1u << 20}));
    return stream;
}

struct FoldSink {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    void on_message(const mog::Message& m) noexcept { hash = mog::trace_hash(m, hash); }
};

void BM_ParseStream(benchmark::State& state) {
    auto& stream = corpus_stream();
    FoldSink warm;
    if (!mog::parse_itch(stream.data(), stream.size(), warm)) {
        state.SkipWithError("parse failed");
        return;
    }
    for (auto _ : state) {
        FoldSink sink;
        if (!mog::parse_itch(stream.data(), stream.size(), sink)) {
            state.SkipWithError("parse failed");
            return;
        }
        benchmark::DoNotOptimize(sink.hash);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<long long>(1u << 20));
}
BENCHMARK(BM_ParseStream);

void BM_ParseStreamBaselineKernel(benchmark::State& state) {
    auto& stream = corpus_stream();
    const auto kernels = mog::kernels_for(mog::Isa::sse4_baseline);
    for (auto _ : state) {
        FoldSink sink;
        if (!mog::parse_itch_as(stream.data(), stream.size(), sink, kernels)) {
            state.SkipWithError("parse failed");
            return;
        }
        benchmark::DoNotOptimize(sink.hash);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<long long>(1u << 20));
}
BENCHMARK(BM_ParseStreamBaselineKernel);

// Single Add Order buffer reused across iterations.
struct AddOrderFixture {
    std::vector<unsigned char> bytes =
        mog::testing::encode_corpus(mog::testing::make_corpus({7, 1}));
};
AddOrderFixture& add_order_fixture() {
    static AddOrderFixture f; // first message of seed 7 is an A
    return f;
}

[[nodiscard]] inline bool decode_add_order_cpp(const unsigned char* buf,
                                               mog::Message& out) noexcept {
    return mog::kernels_for(mog::Isa::sse4_baseline).parse_one(buf, mog::wire::kAddOrderSize, out);
}

#if defined(__x86_64__)
// Hand-written asm twin of the baseline Add Order kernel: identical outputs,
// identical accept/reject semantics. Kept only on a >= 15% measured win.
[[nodiscard]] inline bool decode_add_order_asm(const unsigned char* buf,
                                               mog::Message& out) noexcept {
    if (buf[19] != 'B' && buf[19] != 'S')
        return false;
    std::uint16_t loc, trk, ts_hi;
    std::uint32_t ts_lo, sh, px;
    std::uint64_t ref, sym;
    __asm__ volatile("movbe 1(%[b]), %[loc]\n\t"
                     "movbe 3(%[b]), %[trk]\n\t"
                     "movbe 5(%[b]), %[tsh]\n\t"
                     "movbe 7(%[b]), %k[tsl]\n\t"
                     "movbe 11(%[b]), %[ref]\n\t"
                     "movbe 20(%[b]), %k[sh]\n\t"
                     "movq 24(%[b]), %[sym]\n\t"
                     "movbe 32(%[b]), %k[px]\n\t"
                     : [loc] "=&r"(loc), [trk] "=&r"(trk), [tsh] "=&r"(ts_hi), [tsl] "=&r"(ts_lo),
                       [ref] "=&r"(ref), [sh] "=&r"(sh), [sym] "=&r"(sym), [px] "=&r"(px)
                     : [b] "r"(buf));
    out.type = 'A';
    out.add_order.header.locate = mog::Locate{loc};
    out.add_order.header.tracking = mog::Tracking{trk};
    out.add_order.header.ts_ns =
        mog::TimestampNs{(static_cast<std::uint64_t>(ts_hi) << 32) | ts_lo};
    out.add_order.order_ref = mog::OrderId{ref};
    out.add_order.side = mog::side_from_wire(static_cast<char>(buf[19]));
    out.add_order.shares = mog::Qty{static_cast<std::int64_t>(sh)};
    mog::swar::store64(reinterpret_cast<unsigned char*>(out.add_order.stock.data()), sym);
    out.add_order.price = mog::Price{static_cast<std::int64_t>(px)};
    return true;
}

void BM_DecodeAddOrderAsm(benchmark::State& state) {
    auto& fix = add_order_fixture();
    mog::Message twin{};
    const bool asm_ok = decode_add_order_asm(fix.bytes.data(), twin);
    const auto ref = mog::decode_add_order(fix.bytes.data(), fix.bytes.size());
    const bool identical = asm_ok && ref.has_value() && twin.add_order.header == ref->header &&
                           twin.add_order.order_ref == ref->order_ref &&
                           twin.add_order.side == ref->side &&
                           twin.add_order.shares == ref->shares &&
                           twin.add_order.stock == ref->stock && twin.add_order.price == ref->price;
    if (!identical) {
        state.SkipWithError("asm twin diverged from C++ kernel");
        return;
    }
    mog::Message out{};
    for (auto _ : state) {
        benchmark::DoNotOptimize(decode_add_order_asm(fix.bytes.data(), out));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DecodeAddOrderAsm);
#endif

void BM_DecodeAddOrderCpp(benchmark::State& state) {
    auto& fix = add_order_fixture();
    mog::Message out{};
    for (auto _ : state) {
        benchmark::DoNotOptimize(decode_add_order_cpp(fix.bytes.data(), out));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DecodeAddOrderCpp);

} // namespace

BENCHMARK_MAIN();
