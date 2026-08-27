// Differential fuzz: OrderBook vs naive ReferenceBook over deterministic
// randomized op streams with contract enforcement, invariant audits, L2
// equivalence checks, and delta-ring fold verification.
// Scale via MOG_BOOK_OPS; the M2 gate run uses >= 10M.

#include <mog/AllocGuard.hpp>
#include <mog/Arena.hpp>
#include <mog/Contracts.hpp>
#include <mog/OrderBook.hpp>
#include <mog/SnapshotRing.hpp>

#include <support/CorpusGen.hpp>
#include <support/ReferenceBook.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using mog::BookError;

using mog::OrderBook;
using mog::OrderId;
using mog::Price;
using mog::Qty;
using mog::Side;

constexpr std::size_t kArenaCapacity = 1 << 18;
constexpr std::int64_t kBandLo = 1'000'000;
constexpr std::int64_t kBandHi = 1'100'000;
constexpr std::size_t kLiveSoftCap = 180'000;

enum class OpKind : std::uint8_t { add, execute, cancel, remove, replace };

struct Op {
    OpKind kind;
    std::uint64_t ref = 0;
    std::uint64_t ref2 = 0;
    std::int64_t qty = 0;
    std::int64_t price = 0;
};

int fail(const char* what, long op_idx) noexcept {
    std::fprintf(stderr, "bookdiff FAIL: %s (op #%ld)\n", what, op_idx);
    return 2; // caller may override with detailed dump
}

[[nodiscard]] std::int64_t draw_price(std::uint64_t& rng) noexcept {
    // Cluster near mid so best-level refill scans get deep exercise.
    if (mog::testing::next_splitmix(rng) % 10 < 7)
        return kBandLo + 50'000 - 256 +
               static_cast<std::int64_t>(mog::testing::next_splitmix(rng) % 512);
    return kBandLo +
           static_cast<std::int64_t>(mog::testing::next_splitmix(rng) % (kBandHi - kBandLo));
}

template <class BookT>
[[nodiscard]] mog::BookTick do_op(BookT& book, const Op& op,
                                  mog::inplace_vector<mog::LevelDelta, 3>* deltas) noexcept {
    switch (op.kind) {
    case OpKind::add:
        return book.add(OrderId{op.ref}, (op.ref & 1) != 0 ? Side::buy : Side::sell, Qty{op.qty},
                        Price{op.price}, deltas);
    case OpKind::execute:
        return book.execute(OrderId{op.ref}, Qty{op.qty}, deltas);
    case OpKind::cancel:
        return book.cancel(OrderId{op.ref}, Qty{op.qty}, deltas);
    case OpKind::remove:
        return book.remove(OrderId{op.ref}, deltas);
    case OpKind::replace:
        return book.replace(OrderId{op.ref}, OrderId{op.ref2}, Qty{op.qty}, Price{op.price},
                            deltas);
    }
    MOG_PRE(false);
    return mog::BookTick{};
}

[[nodiscard]] bool same_results(const mog::BookTick& a, const mog::BookTick& b, std::int64_t bb_a,
                                std::int64_t ba_a, std::int64_t bb_b, std::int64_t ba_b,
                                long idx) noexcept {
    if (a.kind != b.kind || a.qty != b.qty || a.error != b.error)
        return false;
    if ((a.kind == mog::kTickError ? 0 : a.price_ticks) !=
        (b.kind == mog::kTickError ? 0 : b.price_ticks))
        return false;
    if (bb_a != bb_b || ba_a != ba_b) {
        std::fprintf(stderr, "best mismatch op#%ld ours(bb=%lld ba=%lld) ref(bb=%lld ba=%lld)\n",
                     idx, static_cast<long long>(bb_a), static_cast<long long>(ba_a),
                     static_cast<long long>(bb_b), static_cast<long long>(ba_b));
        return false;
    }
    return true;
}

[[nodiscard]] bool same_deltas(const mog::inplace_vector<mog::LevelDelta, 3>& a,
                               const mog::inplace_vector<mog::LevelDelta, 3>& b) noexcept {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].side != b[i].side || a[i].price_ticks != b[i].price_ticks ||
            a[i].qty_delta != b[i].qty_delta)
            return false;
    return true;
}

struct Pool {
    std::vector<std::uint64_t> items;
    std::unordered_set<std::uint64_t> seen;

    void insert(std::uint64_t r) noexcept {
        if (seen.insert(r).second)
            items.push_back(r);
    }
    void erase(std::uint64_t r) noexcept {
        auto it = seen.find(r);
        if (it == seen.end())
            return;
        seen.erase(it);
        for (std::size_t i = 0; i < items.size(); ++i)
            if (items[i] == r) {
                items[i] = items.back();
                items.pop_back();
                return;
            }
    }
    [[nodiscard]] bool contains(std::uint64_t r) const noexcept { return seen.count(r) != 0; }
    [[nodiscard]] std::uint64_t pick(std::uint64_t& rng) const noexcept {
        return items[mog::testing::next_splitmix(rng) % items.size()];
    }
    [[nodiscard]] bool empty() const noexcept { return items.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items.size(); }
};

void sync_pool(Pool& pool, const mog::BookTick& t, std::uint64_t ref, std::uint64_t ref2) noexcept {
    using K = mog::BookEventKind;
    switch (static_cast<K>(t.kind)) {
    case K::added:
        pool.insert(ref);
        break;
    case K::reduced:
        break;
    case K::removed:
        pool.erase(ref);
        break;
    case K::replaced:
        pool.erase(ref);
        pool.insert(ref2);
        break;
    }
}

using L2Vec = std::vector<std::tuple<std::uint8_t, std::int64_t, std::int64_t>>;

template <class BookT>
void collect_l2(const BookT& book, L2Vec& out) noexcept {
    out.clear();
    book.for_each_l2([&](Side s, std::int64_t tick, std::int64_t qty) {
        out.emplace_back(static_cast<std::uint8_t>(mog::to_wire(s)), tick, qty);
    });
    std::sort(out.begin(), out.end());
}

} // namespace

int main() {
    mog::contracts::set_mode(mog::contracts::Mode::enforce);

    std::size_t ops_target = 10'000'000;
    if (const char* n = std::getenv("MOG_BOOK_OPS"); n != nullptr && *n != '\0')
        ops_target = static_cast<std::size_t>(std::strtoull(n, nullptr, 10));

    OrderBook<> book({.arena_capacity = kArenaCapacity,
                      .ladder = {.lo_tick = kBandLo, .hi_tick = kBandHi, .page_pool = 256}});
    mog::testing::ReferenceBook ref;
    Pool pool;
    // Static: ~8 MB of ring storage would blow the default stack in main().
    static mog::DeltaRing<1 << 18> ring;
    constexpr std::size_t kRingVerifyOps = 100'000;
    std::uint64_t trace = 0xcbf29ce484222325ULL;

    std::uint64_t rng = 0xB00DC0DEULL;
    std::uint64_t fresh_ref = 1;
    Op recent[8]{};
    std::size_t recent_n = 0;
    L2Vec ours, theirs;

    for (std::size_t step = 0; step < ops_target; ++step) {
        const std::uint64_t roll = mog::testing::next_splitmix(rng);
        const std::uint64_t sel = roll % 100;
        const auto known = [&]() -> std::uint64_t {
            return !pool.empty() && mog::testing::next_splitmix(rng) % 100 < 70
                       ? pool.pick(rng)
                       : mog::testing::next_splitmix(rng);
        };
        const std::uint64_t qty_draw = mog::testing::next_splitmix(rng);

        Op op{};
        if (sel < 35 && pool.size() <= kLiveSoftCap) {
            op.kind = OpKind::add;
            op.ref = fresh_ref++;
            op.qty = static_cast<std::int64_t>(qty_draw % 900) + 1;
            op.price = draw_price(rng);
        } else if (sel < 55) {
            op.kind = OpKind::execute;
            op.ref = known();
            op.qty = static_cast<std::int64_t>(qty_draw % 900) + 1;
        } else if (sel < 65) {
            op.kind = OpKind::cancel;
            op.ref = known();
            op.qty = static_cast<std::int64_t>(qty_draw % 900) + 1;
        } else if (sel < 80) {
            op.kind = OpKind::remove;
            op.ref = known();
        } else {
            op.kind = OpKind::replace;
            op.ref = known();
            op.ref2 = mog::testing::next_splitmix(rng) % 20 == 0 && !pool.empty() ? pool.pick(rng)
                                                                                  : fresh_ref++;
            op.qty = static_cast<std::int64_t>(qty_draw % 900) + 1;
            op.price = draw_price(rng);
        }

        if (recent_n < 8) {
            recent[recent_n] = op;
        } else {
            recent[recent_n % 8] = op;
        }
        ++recent_n;
        mog::inplace_vector<mog::LevelDelta, 3> got_deltas{}, want_deltas{};
        mog::alloc::arm();
        const mog::BookTick got = do_op(book, op, &got_deltas);
        const bool alloc_free = mog::alloc::disarm();
        if (!alloc_free)
            return fail("heap allocation inside replay op", static_cast<long>(step));
        const mog::BookTick want = do_op(ref, op, &want_deltas);

        if (mog::ok(got) != mog::ok(want))
            return fail("accept/reject divergence", static_cast<long>(step));
        if (!mog::ok(got)) {
            if (got.error != want.error)
                return fail("error code divergence", static_cast<long>(step));
            continue;
        }
        if (!same_deltas(got_deltas, want_deltas))
            return fail("delta divergence", static_cast<long>(step));
        if (!same_results(got, want, book.best_bid(), book.best_ask(), ref.best_bid(),
                          ref.best_ask(), static_cast<long>(step))) {
            for (std::size_t i = 0; i < recent_n && i < 8; ++i) {
                const Op& d = recent[(recent_n + i) % 8];
                std::fprintf(stderr, "op kind=%u ref=%llu ref2=%llu qty=%lld px=%lld\n",
                             static_cast<unsigned>(d.kind), static_cast<unsigned long long>(d.ref),
                             static_cast<unsigned long long>(d.ref2), static_cast<long long>(d.qty),
                             static_cast<long long>(d.price));
            }
            return fail("result divergence", static_cast<long>(step));
        }

        sync_pool(pool, got, op.ref, op.ref2);

        for (const mog::LevelDelta& d : got_deltas) {
            trace = (trace ^ static_cast<std::uint64_t>(d.side)) * 0x100000001b3ULL;
            trace = (trace ^ static_cast<std::uint64_t>(d.price_ticks)) * 0x100000001b3ULL;
            trace = (trace ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(d.qty_delta))) *
                    0x100000001b3ULL;
            if (step < kRingVerifyOps)
                ring.push(d);
        }

        if (step % 997 == 0) {
            if (!book.audit())
                return fail("invariant audit", static_cast<long>(step));
            collect_l2(book, ours);
            collect_l2(ref, theirs);
            if (ours != theirs)
                return fail("full L2 divergence", static_cast<long>(step));
            if (book.live_orders() != pool.size())
                return fail("live-count drift vs driver pool", static_cast<long>(step));
        }

        if (step == kRingVerifyOps - 1) {
            if (ring.dropped_count() != 0)
                return fail("ring dropped transitions before verification",
                            static_cast<long>(step));
            std::map<std::pair<std::uint8_t, std::int64_t>, std::int64_t> folded;
            ring.for_each([&](const mog::Transition& t) {
                folded[{t.delta.side, t.delta.price_ticks}] += t.delta.qty_delta;
            });
            std::map<std::pair<std::uint8_t, std::int64_t>, std::int64_t> actual;
            book.for_each_l2([&](Side s, std::int64_t tick, std::int64_t qty) {
                actual[{static_cast<std::uint8_t>(mog::to_wire(s)), tick}] = qty;
            });
            if (folded != actual)
                return fail("delta-ring fold != live L2", static_cast<long>(step));
        }
    }

    if (!book.audit())
        return fail("final invariant audit", static_cast<long>(ops_target));
    if (book.live_orders() != ref.live_orders())
        return fail("final live-count divergence", static_cast<long>(ops_target));

    std::printf("bookdiff OK (%zu ops, live=%zu, trace=%016llx)\n", ops_target, book.live_orders(),
                static_cast<unsigned long long>(trace));
    return 0;
}
