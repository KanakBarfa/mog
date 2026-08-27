// End-to-end: ITCH capture -> decode -> OrderBook vs ReferenceBook, driven
// identically over data/sample.itch, with a determinism double-run and an
// out-of-band rejection probe.

#include <mog/AllocGuard.hpp>
#include <mog/Contracts.hpp>
#include <mog/ITCHParser.hpp>
#include <mog/OrderBook.hpp>

#include <support/CorpusGen.hpp>
#include <support/ReferenceBook.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <tuple>
#include <vector>

namespace {

constexpr std::int64_t kBandLo = 0;
constexpr std::int64_t kBandHi = 12'000'000;

int fail(const char* what) noexcept {
    std::fprintf(stderr, "booke2e FAIL: %s\n", what);
    return 1;
}

template <class BookT>
void collect_l2(const BookT& book,
                std::vector<std::tuple<std::uint8_t, std::int64_t, std::int64_t>>& out) noexcept {
    out.clear();
    book.for_each_l2([&](mog::Side s, std::int64_t tick, std::int64_t qty) {
        out.emplace_back(static_cast<std::uint8_t>(mog::to_wire(s)), tick, qty);
    });
    std::sort(out.begin(), out.end());
}

[[nodiscard]] std::vector<mog::Message> load_capture(const char* path) noexcept {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
        return {};
    std::vector<unsigned char> bytes;
    unsigned char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);

    std::vector<mog::Message> msgs;
    std::size_t off = 0;
    while (off < bytes.size()) {
        auto decoded = mog::decode_message(bytes.data() + off, bytes.size() - off);
        if (!decoded)
            return {};
        const std::uint8_t need = mog::kFrame.length[bytes[off]];
        off += need;
        msgs.push_back(*decoded);
    }
    return msgs;
}

[[nodiscard]] std::uint64_t run_pipeline(const std::vector<mog::Message>& msgs) noexcept {
    // Arena scales with capture size: the reference book is unbounded, so a
    // fixed small arena would diverge on arena-full rejects for large inputs.
    std::size_t cap = 1;
    while (cap < msgs.size() && cap < (std::size_t{1} << 22))
        cap <<= 1;
    mog::OrderBook<> book({.arena_capacity = cap,
                           .ladder = {.lo_tick = kBandLo, .hi_tick = kBandHi, .page_pool = 512}});
    mog::testing::ReferenceBook ref;
    std::uint64_t trace = 0xcbf29ce484222325ULL;

    for (const mog::Message& m : msgs) {
        mog::alloc::arm();
        mog::inplace_vector<mog::LevelDelta, 3> got_deltas{}, want_deltas{};
        const mog::BookTick got = book.apply(m, &got_deltas);
        const bool alloc_free = mog::alloc::disarm();
        if (!alloc_free) {
            std::fprintf(stderr, "booke2e FAIL: heap allocation during replay\n");
            std::exit(1);
        }
        const mog::BookTick want = ref.apply(m, &want_deltas);
        if (mog::ok(got) != mog::ok(want)) {
            std::fprintf(stderr, "booke2e FAIL: accept/reject at type '%c'\n", m.type);
            std::exit(1);
        }
        if (!mog::ok(got))
            continue;
        if (!(got.kind == want.kind && got.qty == want.qty && book.best_bid() == ref.best_bid() &&
              book.best_ask() == ref.best_ask() && got_deltas.size() == want_deltas.size())) {
            std::fprintf(
                stderr,
                "booke2e FAIL: result at type '%c' kind %u/%u qty %u/%u "
                "bb %lld/%lld ba %lld/%lld deltas %zu/%zu\n",
                m.type, got.kind, want.kind, got.qty, want.qty,
                static_cast<long long>(book.best_bid()), static_cast<long long>(ref.best_bid()),
                static_cast<long long>(book.best_ask()), static_cast<long long>(ref.best_ask()),
                got_deltas.size(), want_deltas.size());
            std::exit(1);
        }
        for (std::size_t i = 0; i < got_deltas.size(); ++i) {
            const mog::LevelDelta& x = got_deltas[i];
            const mog::LevelDelta& y = want_deltas[i];
            if (x.side != y.side || x.price_ticks != y.price_ticks || x.qty_delta != y.qty_delta) {
                std::fprintf(
                    stderr,
                    "booke2e FAIL: delta[%zu] at type '%c': "
                    "%c %lld %+lld vs %c %lld %+lld\n",
                    i, m.type, static_cast<char>(x.side), static_cast<long long>(x.price_ticks),
                    static_cast<long long>(x.qty_delta), static_cast<char>(y.side),
                    static_cast<long long>(y.price_ticks), static_cast<long long>(y.qty_delta));
                std::exit(1);
            }
        }
        if (mog::ok(got))
            for (const mog::LevelDelta& d : got_deltas) {
                trace = (trace ^ static_cast<std::uint64_t>(d.side)) * 0x100000001b3ULL;
                trace = (trace ^ static_cast<std::uint64_t>(d.price_ticks)) * 0x100000001b3ULL;
                trace =
                    (trace ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(d.qty_delta))) *
                    0x100000001b3ULL;
            }
    }

    if (!book.audit()) {
        std::fprintf(stderr, "booke2e FAIL: audit\n");
        std::exit(1);
    }

    std::vector<std::tuple<std::uint8_t, std::int64_t, std::int64_t>> ours, theirs;
    collect_l2(book, ours);
    collect_l2(ref, theirs);
    if (ours != theirs) {
        std::fprintf(stderr, "booke2e FAIL: final L2 divergence (%zu vs %zu levels)\n", ours.size(),
                     theirs.size());
        for (std::size_t i = 0; i < ours.size() || i < theirs.size(); ++i) {
            const auto o = i < ours.size()
                               ? ours[i]
                               : std::make_tuple(std::uint8_t{0}, std::int64_t{0}, std::int64_t{0});
            const auto t = i < theirs.size()
                               ? theirs[i]
                               : std::make_tuple(std::uint8_t{0}, std::int64_t{0}, std::int64_t{0});
            std::fprintf(stderr, "  [%zu] ours %c %lld x%lld | ref %c %lld x%lld\n", i,
                         static_cast<char>(std::get<0>(o)), static_cast<long long>(std::get<1>(o)),
                         static_cast<long long>(std::get<2>(o)), static_cast<char>(std::get<0>(t)),
                         static_cast<long long>(std::get<1>(t)),
                         static_cast<long long>(std::get<2>(t)));
        }
        std::exit(1);
    }
    return trace;
}

} // namespace

int main(int argc, char** argv) {
    mog::contracts::set_mode(mog::contracts::Mode::enforce);
    const char* path = MOG_SAMPLE_PATH;
    if (argc > 1)
        path = argv[1];

    const auto msgs = load_capture(path);
    if (msgs.empty())
        return fail("capture missing or undecodable");

    std::size_t accepted = 0;
    for (const mog::Message& m : msgs) {
        switch (m.type) {
        case 'A':
        case 'F':
        case 'E':
        case 'C':
        case 'X':
        case 'D':
        case 'U':
            ++accepted;
            break;
        default:
            break;
        }
    }
    if (accepted != msgs.size())
        return fail("unexpected message type mix");

    const std::uint64_t t1 = run_pipeline(msgs);
    const std::uint64_t t2 = run_pipeline(msgs);
    if (t1 != t2)
        return fail("nondeterministic trace across runs");

    mog::OrderBook<> probe(
        {.arena_capacity = 64, .ladder = {.lo_tick = kBandLo, .hi_tick = kBandHi, .page_pool = 4}});
    const mog::BookTick rejected =
        probe.add(mog::OrderId{1}, mog::Side::buy, mog::Qty{100}, mog::Price{kBandHi + 5});
    if (mog::ok(rejected) ||
        rejected.error != static_cast<std::uint8_t>(mog::BookError::out_of_band))
        return fail("out-of-band add not rejected");
    const mog::BookTick tiny =
        probe.add(mog::OrderId{1}, mog::Side::buy, mog::Qty{0}, mog::Price{100});
    if (mog::ok(tiny) || tiny.error != static_cast<std::uint8_t>(mog::BookError::bad_qty))
        return fail("zero-qty add not rejected");
    // Ref 0 is the unset sentinel throughout the codebase; booking it used to
    // succeed and then fail audit() - found by the libFuzzer book target.
    const mog::BookTick sentinel =
        probe.add(mog::OrderId{0}, mog::Side::buy, mog::Qty{10}, mog::Price{100});
    if (mog::ok(sentinel) || sentinel.error != static_cast<std::uint8_t>(mog::BookError::zero_ref))
        return fail("ref-0 add not rejected");

    // Construct/destroy cycles with heap churn between epochs: a book built on
    // recycled allocator memory must behave identically to one on fresh pages.
    // Regression guard for the ladder page-pool occupancy sweep (the pool is raw
    // operator-new memory; Page default initializers never run there).
    {
        const auto churn = [] {
            std::vector<std::vector<int>> junk;
            for (int i = 0; i < 64; ++i)
                junk.emplace_back(4096, 0x7F);
            return junk.size();
        };
        for (int epoch = 0; epoch < 8; ++epoch) {
            static_cast<void>(churn());
            mog::OrderBook<> recycled(
                {.arena_capacity = 4096,
                 .ladder = {.lo_tick = kBandLo, .hi_tick = kBandHi, .page_pool = 16}});
            for (int step = 0; step < 200; ++step) {
                const std::uint64_t ustep = static_cast<std::uint64_t>(static_cast<unsigned>(step));
                std::uint64_t rng =
                    0x5EED0000ULL +
                    static_cast<std::uint64_t>(static_cast<unsigned>(epoch)) * 7919U + ustep;
                const std::uint64_t r = mog::testing::next_splitmix(rng);
                const std::int64_t px = kBandLo + 100 + static_cast<std::int64_t>(r % 400);
                if (step % 2 == 0) {
                    static_cast<void>(
                        recycled.add(mog::OrderId{static_cast<std::uint64_t>(step + 1)},
                                     (r & 1) != 0 ? mog::Side::buy : mog::Side::sell, mog::Qty{25},
                                     mog::Price{px}));
                } else {
                    static_cast<void>(
                        recycled.remove(mog::OrderId{static_cast<std::uint64_t>(step)}));
                }
            }
            if (!recycled.audit())
                return fail("audit failed on recycled-heap book");
        }
    }

    std::printf("booke2e OK (%zu messages, trace=%016llx)\n", msgs.size(),
                static_cast<unsigned long long>(t1));
    return 0;
}
