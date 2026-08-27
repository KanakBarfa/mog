// RFC-0001 exhaustive twin of specs/matching/BookMatching.tla.
//
// Drives OrderBook<> through the same four actions the TLA+ model defines
// (Rest / Cancel / Deplete / MarketTake) and enumerates the FULL reachable
// state set by depth-first search over action traces, asserting the same
// invariants TLC checks: audit() sanity, conservation between FIFO queues
// and ladder aggregates, no crossed book, ref uniqueness, non-negative
// aggregates.
//
// This is the JVM-free half of the formal-model slice: ctest proves the
// semantics on every machine; CI's TLC run proves the spec where Java
// exists. Both files carry the RFC-0001 tag; review is the drift backstop.
#include <mog/OrderBook.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using mog::BookTick;
using mog::kNoTick;
using mog::kNullIndex;
using mog::ok;
using mog::OrderId;
using mog::Price;
using mog::Qty;
using mog::Side;

constexpr std::int64_t kLo = 997;
constexpr std::int64_t kHi = 1000; // exclusive
constexpr std::int64_t kPrices[3]{997, 998, 999};
constexpr int kMaxQty = 2;
constexpr int kDepthLimit = 12;

constexpr OrderId kRefs[3]{OrderId{11}, OrderId{12}, OrderId{13}};
constexpr Side kSides[2]{Side::buy, Side::sell};

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

mog::PriceLadder<>::Config band() noexcept {
    return mog::PriceLadder<>::Config{kLo, kHi, 4};
}

struct Action {
    enum class Kind : std::uint8_t { rest = 0, cancel = 1, deplete = 2, take = 3 } kind;
    int ref_i = 0;
    int side_i = 0;
    int price_i = 0;
    int qty = 1; // for rest/deplete
};

mog::OrderBook<>::Config book_config() noexcept {
    mog::OrderBook<>::Config cfg{};
    cfg.arena_capacity = 16;
    cfg.ladder = band();
    return cfg;
}

// ---- canonical serialization for dedup --------------------------------------
std::string serialize(const mog::OrderBook<>& b) {
    std::string s;
    char buf[48];
    for (const Side sd : kSides) {
        s += sd == Side::buy ? 'B' : 'S';
        for (const std::int64_t p : kPrices) {
            b.for_each_in_level(sd, Price{p}, [&](OrderId ref, std::int64_t rem) {
                std::snprintf(buf, sizeof buf, "%llu:%lld,",
                              static_cast<unsigned long long>(ref.value),
                              static_cast<long long>(rem));
                s += buf;
            });
            s += '|';
        }
    }
    return s;
}

// ---- invariant checks (mirror Inv == TypeOK /\ Conservation /\ ...) ---------
bool check_invariants(const mog::OrderBook<>& b) {
    bool good = true;
    const bool audit_ok = b.audit();
    if (!audit_ok)
        std::fprintf(stderr, "  [viol] audit()\n");
    good &= audit_ok;

    const std::int64_t bb = b.best_bid(), ba = b.best_ask();
    if (bb != kNoTick && ba != kNoTick && bb >= ba) {
        std::fprintf(stderr, "  [viol] crossed book: bid=%lld ask=%lld\n",
                     static_cast<long long>(bb), static_cast<long long>(ba));
        good = false; // NoCrossedBook
    }

    for (const Side sd : kSides) { // Conservation
        for (const std::int64_t p : kPrices) {
            std::int64_t sum = 0;
            b.for_each_in_level(sd, Price{p}, [&](OrderId, std::int64_t rem) { sum += rem; });
            if (sum != b.qty_at(sd, Price{p})) {
                std::fprintf(stderr, "  [viol] conservation %c@%lld: queues=%lld agg=%lld\n",
                             sd == Side::buy ? 'B' : 'S', static_cast<long long>(p),
                             static_cast<long long>(sum),
                             static_cast<long long>(b.qty_at(sd, Price{p})));
                good = false;
            }
        }
    }

    std::unordered_set<std::uint64_t> seen; // RefUnique + QtyPositive
    for (const Side sd : kSides)
        for (const std::int64_t p : kPrices)
            b.for_each_in_level(sd, Price{p}, [&](OrderId ref, std::int64_t rem) {
                if (rem <= 0 || !seen.insert(ref.value).second) {
                    std::fprintf(stderr, "  [viol] ref/qty ref=%llu rem=%lld\n",
                                 static_cast<unsigned long long>(ref.value),
                                 static_cast<long long>(rem));
                    good = false;
                }
            });
    return good;
}

// ---- action guards mirroring the .tla preconditions -------------------------
[[nodiscard]] bool would_cross(const mog::OrderBook<>& b, Side s, std::int64_t p) {
    if (s == Side::buy) {
        const std::int64_t a = b.best_ask();
        return a != kNoTick && a <= p;
    }
    const std::int64_t t = b.best_bid();
    return t != kNoTick && t >= p;
}

void apply(mog::OrderBook<>& b, const Action& a) {
    switch (a.kind) {
    case Action::Kind::rest:
        static_cast<void>(
            b.add(kRefs[a.ref_i], kSides[a.side_i], Qty{a.qty}, Price{kPrices[a.price_i]}));
        break;
    case Action::Kind::cancel:
        static_cast<void>(b.remove(kRefs[a.ref_i]));
        break;
    case Action::Kind::deplete: {
        const Side s = kSides[a.side_i];
        const Price p{kPrices[a.price_i]};
        std::int64_t left = a.qty;
        while (left > 0) {
            const std::uint64_t head = b.front_ref(s, p);
            if (head == 0)
                break;
            const std::int64_t take = std::min(left, b.remaining_of(OrderId{head}));
            OrderId victim{};
            static_cast<void>(b.execute_front(s, p, Qty{take}, nullptr, &victim));
            left -= take;
        }
        break;
    }
    case Action::Kind::take: {
        const Side s = kSides[a.side_i];
        const Side opp = s == Side::buy ? Side::sell : Side::buy;
        for (const std::int64_t p : kPrices) {
            const bool through = s == Side::buy ? kPrices[a.price_i] >= p : kPrices[a.price_i] <= p;
            if (!through)
                continue;
            // execute_front is strictly per-event: each call consumes from
            // the current head only, so clear the level head by head.
            while (b.qty_at(opp, Price{p}) > 0) {
                const std::uint64_t head = b.front_ref(opp, Price{p});
                const std::int64_t rem = b.remaining_of(OrderId{head});
                OrderId victim{};
                static_cast<void>(b.execute_front(opp, Price{p}, Qty{rem}, nullptr, &victim));
            }
        }
        break;
    }
    }
}

std::vector<Action> successors(const mog::OrderBook<>& b) {
    std::vector<Action> out;
    auto ref_used = [&](int i) { return b.find_handle(kRefs[i]).index != kNullIndex; };
    for (int ri = 0; ri < 3; ++ri) { // Rest
        for (int si = 0; si < 2; ++si)
            for (int pi = 0; pi < 3; ++pi)
                for (int q = 1; q <= kMaxQty; ++q) {
                    if (ref_used(ri))
                        continue;
                    if (would_cross(b, kSides[si], kPrices[pi]))
                        continue;
                    out.push_back(Action{Action::Kind::rest, ri, si, pi, q});
                }
    }
    for (int ri = 0; ri < 3; ++ri) // Cancel
        if (ref_used(ri))
            out.push_back(Action{Action::Kind::cancel, ri});
    for (int si = 0; si < 2; ++si) // Deplete
        for (int pi = 0; pi < 3; ++pi)
            for (int q = 1; q <= kMaxQty; ++q)
                if (b.qty_at(kSides[si], Price{kPrices[pi]}) > 0)
                    out.push_back(Action{Action::Kind::deplete, 0, si, pi, q});
    for (int si = 0; si < 2; ++si) // MarketTake
        for (int pi = 0; pi < 3; ++pi) {
            const Side opp = kSides[si] == Side::buy ? Side::sell : Side::buy;
            for (const std::int64_t p : kPrices) {
                const bool through = kSides[si] == Side::buy ? kPrices[pi] >= p : kPrices[pi] <= p;
                if (through && b.qty_at(opp, Price{p}) > 0) {
                    out.push_back(Action{Action::Kind::take, 0, si, pi});
                    break;
                }
            }
        }
    return out;
}

} // namespace

int main() {
    std::size_t states = 0;
    std::unordered_set<std::string> visited;

    // DFS over action traces; books are rebuilt from traces because
    // OrderBook<> is deliberately non-copyable. Branching is tiny, so this
    // stays well under the ctest budget.
    struct Frame {
        std::vector<Action> trace;
    };
    std::vector<Frame> stack{{}};

    std::size_t steps = 0;
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (static_cast<int>(frame.trace.size()) > kDepthLimit)
            continue;

        mog::OrderBook<> b{book_config()};
        for (const Action& a : frame.trace)
            apply(b, a);

        if (!check_invariants(b)) {
            std::fprintf(stderr, "invariant violated at depth %zu, trace:\n", frame.trace.size());
            for (const Action& a : frame.trace)
                std::fprintf(stderr, "  kind=%d ref_i=%d side_i=%d px=%lld qty=%d\n",
                             static_cast<int>(a.kind), a.ref_i, a.side_i,
                             static_cast<long long>(kPrices[a.price_i]), a.qty);
            std::fprintf(stderr, "state: %s\n", serialize(b).c_str());
            ++failures;
            break;
        }
        if (!visited.insert(serialize(b)).second)
            continue;
        ++states;
        for (const Action& a : successors(b)) {
            Frame next;
            next.trace = frame.trace;
            next.trace.push_back(a);
            stack.push_back(std::move(next));
        }
        CHECK(++steps < 50'000'000); // runaway guard
    }

    CHECK(states > 100); // the space must actually be explored, not vacuous

    if (failures == 0)
        std::printf("matching_modelcheck: %zu reachable states, all invariants hold\n", states);
    else
        std::printf("matching_modelcheck: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
