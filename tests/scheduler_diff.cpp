// M3 scheduler verification: unit probes over both engines (binary heap and
// hierarchical timing wheel), differential fuzz against a naive sorted-vector
// reference plus a cross-engine mirror, and the determinism harness (repeated
// replays must produce identical SHA-256 pop-stream digests, and both engines
// must produce the same digest).
#include <mog/Scheduler.hpp>
#include <mog/Sha256.hpp>
#include <mog/TimingWheel.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

struct Payload {
    std::uint64_t a;
    std::uint32_t b;
    friend bool operator==(const Payload&, const Payload&) = default;
};

constexpr std::size_t kCapacity = 2048;

struct RefEvent {
    std::uint64_t ts;
    std::uint64_t ord; // insertion ordinal among successful pushes
    Payload value;
};
// Naive mirror: vector kept sorted by (ts, ord); O(n) inserts are fine here.
class Reference {
public:
    void push(std::uint64_t ts, std::uint64_t ord, const Payload& v) {
        RefEvent e{ts, ord, v};
        std::size_t i = events_.size();
        events_.push_back(e);
        while (i > 0 && less(events_[i], events_[i - 1])) {
            std::swap(events_[i], events_[i - 1]);
            --i;
        }
    }
    bool cancel_ord(std::uint64_t ord) {
        for (std::size_t i = 0; i < events_.size(); ++i)
            if (events_[i].ord == ord) {
                events_.erase(events_.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        return false;
    }
    [[nodiscard]] bool empty() const noexcept { return events_.empty(); }
    [[nodiscard]] const RefEvent& front() const noexcept { return events_.front(); }
    void pop() { events_.erase(events_.begin()); }
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }

private:
    static bool less(const RefEvent& x, const RefEvent& y) noexcept {
        return x.ts < y.ts || (x.ts == y.ts && x.ord < y.ord);
    }
    std::vector<RefEvent> events_;
};

struct OpRecord {
    std::uint8_t kind; // 0=push 1=pop 2=cancel-random-slot
    std::uint64_t ts;
    std::uint64_t a;
    std::uint32_t b;
};

void fold(mog::Sha256& sha, std::uint64_t ts, const Payload& p) noexcept {
    unsigned char bytes[20];
    for (int i = 0; i < 8; ++i)
        bytes[i] = static_cast<unsigned char>(ts >> (8 * i));
    for (int i = 0; i < 8; ++i)
        bytes[8 + i] = static_cast<unsigned char>(p.a >> (8 * i));
    for (int i = 0; i < 4; ++i)
        bytes[16 + i] = static_cast<unsigned char>(p.b >> (8 * i));
    sha.update(bytes, sizeof(bytes));
}

template <class Eng>
std::array<unsigned char, 32> replay_and_hash(const std::vector<OpRecord>& corpus) {
    Eng eng(kCapacity);
    mog::Sha256 sha;

    for (std::size_t step = 0; step < corpus.size(); ++step) {
        const OpRecord& op = corpus[step];
        switch (op.kind) {
        case 0:
            if (eng.size() < kCapacity)
                static_cast<void>(eng.push(op.ts, Payload{op.a, op.b}));
            break;
        case 1:
            if (!eng.empty()) {
                const std::uint64_t ts = eng.peek_min_ts();
                fold(sha, ts, eng.pop_min());
            }
            break;
        default:
            break; // cancellations covered by the differential phases
        }
        if (step % 997 == 0 && !eng.audit()) {
            std::fputs("audit failed mid-replay\n", stderr);
            std::exit(2);
        }
    }
    while (!eng.empty()) {
        const std::uint64_t ts = eng.peek_min_ts();
        fold(sha, ts, eng.pop_min());
    }
    return sha.finish();
}

// Runs one probe scenario against each engine; F returns nonzero on failure.
template <class Eng, class Probe>
bool run_probe(const char* name, Probe&& probe) {
    Eng eng(kCapacity);
    if (probe(eng) != 0) {
        std::fprintf(stderr, "FAIL %s\n", name);
        return false;
    }
    return true;
}

template <class Probe>
bool for_each_engine(const char* name, Probe&& probe) {
    return run_probe<mog::Scheduler<Payload>>(name, probe) &&
           run_probe<mog::TimingWheel<Payload>>(name, probe);
}

} // namespace

int main() {
    // Phase 0: SHA-256 known-answer vectors (NIST) guard the trace codec.
    {
        mog::Sha256 sha;
        const auto empty_digest = sha.finish();
        sha.reset();
        sha.update("abc", 3);
        const auto abc = sha.finish();
        const auto to_hex = [](const std::array<unsigned char, 32>& d) {
            char scratch[65];
            const auto v = mog::Sha256::hex(d, scratch);
            return std::string(v);
        };
        if (to_hex(empty_digest) !=
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
            to_hex(abc) != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
            std::fputs("FAIL sha256 known-answer\n", stderr);
            return 1;
        }
    }

    // Phase 1: unit probes on hand-built scenarios, run against both engines.

    // Equal timestamps pop strictly in push order (stable tie-break).
    if (!for_each_engine("equal-ts stability", [](auto& s) {
            for (int i = 0; i < 100; ++i)
                static_cast<void>(s.push(42, Payload{static_cast<std::uint64_t>(i), 0}));
            for (int i = 0; i < 100; ++i)
                if (!(s.pop_min() == Payload{static_cast<std::uint64_t>(i), 0}))
                    return 1;
            return 0;
        }))
        return 1;

    // Stale handles stop validating after pop or cancel.
    if (!for_each_engine("stale handles", [](auto& s) {
            using H = decltype(s.push(1, Payload{}));
            const H h1 = s.push(1, Payload{10, 0});
            if (!(s.pop_min() == Payload{10, 0}) || s.cancel(h1))
                return 1;
            const H h2 = s.push(1, Payload{11, 0});
            if (!s.cancel(h2) || s.cancel(h2))
                return 1;
            return 0;
        }))
        return 1;

    // Far-future pushes do not disturb near-term drains (all pushed up front,
    // which also keeps the timing wheel's past-guard satisfied).
    if (!for_each_engine("far-future interleave", [](auto& s) {
            static_cast<void>(s.push(1000, Payload{3, 0}));
            static_cast<void>(s.push(5, Payload{1, 0}));
            static_cast<void>(s.push(999999999ULL, Payload{4, 0}));
            static_cast<void>(s.push(6, Payload{2, 0}));
            for (std::uint64_t want = 1; want <= 4; ++want)
                if (!(s.pop_min() == Payload{want, 0}))
                    return 1;
            return 0;
        }))
        return 1;

    // clear() resets fully; generations bump so old handles stay dead.
    if (!for_each_engine("clear semantics", [](auto& s) {
            const auto h = s.push(7, Payload{1, 0});
            s.clear();
            if (!s.empty() || !s.audit() || s.cancel(h))
                return 1;
            static_cast<void>(s.push(7, Payload{2, 0}));
            if (!(s.pop_min() == Payload{2, 0}) || !s.audit())
                return 1;
            return 0;
        }))
        return 1;

    // Slot 63 and multi-level boundary demotion.
    if (!for_each_engine("slot-63 boundaries", [](auto& s) {
            static_cast<void>(s.push(63, Payload{1, 0}));
            static_cast<void>(s.push(64, Payload{2, 0}));
            static_cast<void>(s.push(127, Payload{3, 0}));
            static_cast<void>(s.push(4095, Payload{4, 0}));
            static_cast<void>(s.push(4096, Payload{5, 0}));
            static_cast<void>(s.push(262143, Payload{6, 0}));
            if (!(s.pop_min() == Payload{1, 0}))
                return 1;
            if (!(s.pop_min() == Payload{2, 0}))
                return 1;
            if (!(s.pop_min() == Payload{3, 0}))
                return 1;
            if (!(s.pop_min() == Payload{4, 0}))
                return 1;
            if (!(s.pop_min() == Payload{5, 0}))
                return 1;
            if (!(s.pop_min() == Payload{6, 0}))
                return 1;
            if (!s.audit())
                return 1;
            return 0;
        }))
        return 1;

    std::puts("unit probes OK");

    // Phase 2a: differential fuzz of the binary heap against the reference,
    // unconstrained push timestamps.
    {
        std::mt19937_64 rng(20260823);
        mog::Scheduler<Payload> sched(kCapacity);
        Reference ref;
        std::vector<mog::EventHandle> handle_of(kCapacity);
        std::vector<std::uint64_t> ord_of_slot(kCapacity, ~std::uint64_t{0});
        std::uint64_t next_ord = 0, pops = 0, cancels_ok = 0;

        for (std::uint64_t step = 0; step < 400000; ++step) {
            const int roll = static_cast<int>(rng() % 100);
            if ((roll < 55 || ref.empty()) && sched.size() < kCapacity) {
                const std::uint64_t ts = rng() % 1000000;
                Payload v{rng(), static_cast<std::uint32_t>(rng())};
                const auto h = sched.push(ts, v);
                ref.push(ts, next_ord, v);
                handle_of[h.index] = h;
                ord_of_slot[h.index] = next_ord++;
            } else if (roll < 80 || (sched.size() >= kCapacity && roll < 95)) {
                if (sched.empty())
                    continue;
                const Payload got = sched.pop_min();
                const RefEvent want = ref.front();
                if (!(got == want.value))
                    return std::fprintf(stderr, "heap pop mismatch at step %llu\n",
                                        static_cast<unsigned long long>(step)),
                           1;
                for (std::size_t i = 0; i < kCapacity; ++i)
                    if (ord_of_slot[i] == want.ord) {
                        ord_of_slot[i] = ~std::uint64_t{0};
                        break;
                    }
                ref.pop();
                ++pops;
            } else if (roll < 95) {
                const std::uint32_t idx = static_cast<std::uint32_t>(rng() % kCapacity);
                const bool got = sched.cancel(handle_of[idx]);
                const bool want =
                    ord_of_slot[idx] != ~std::uint64_t{0} && ref.cancel_ord(ord_of_slot[idx]);
                if (got != want)
                    return std::fprintf(stderr, "heap cancel mismatch at step %llu\n",
                                        static_cast<unsigned long long>(step)),
                           1;
                if (got) {
                    ord_of_slot[idx] = ~std::uint64_t{0};
                    ++cancels_ok;
                }
            } else if (!sched.empty()) {
                if (sched.peek_min_ts() != ref.front().ts ||
                    !(sched.peek_min() == ref.front().value))
                    return std::fprintf(stderr, "heap peek mismatch at step %llu\n",
                                        static_cast<unsigned long long>(step)),
                           1;
            }
            if (step % 997 == 0 && !sched.audit())
                return std::fprintf(stderr, "heap audit failed at step %llu\n",
                                    static_cast<unsigned long long>(step)),
                       1;
        }
        if (sched.size() != ref.size() || !sched.audit()) {
            std::fputs("heap final state mismatch\n", stderr);
            return 1;
        }
        std::printf("heap differential OK (pops=%llu cancels=%llu)\n",
                    static_cast<unsigned long long>(pops),
                    static_cast<unsigned long long>(cancels_ok));
    }

    // Phase 2b: cross-engine mirror with non-decreasing feeds (the timing
    // wheel's contract): identical op streams must produce identical pop
    // streams across heap and wheel, checked against the shared reference.
    {
        std::mt19937_64 rng(20260824);
        mog::Scheduler<Payload> heap(kCapacity);
        mog::TimingWheel<Payload> wheel(kCapacity);
        Reference ref;
        std::vector<mog::EventHandle> heap_h(kCapacity), wheel_h(kCapacity);
        std::vector<std::uint64_t> ord_of_slot(kCapacity, ~std::uint64_t{0});
        std::uint64_t next_ord = 0, ts_cursor = 0, pops = 0, cancels_ok = 0;

        for (std::uint64_t step = 0; step < 400000; ++step) {
            const int roll = static_cast<int>(rng() % 100);
            if ((roll < 55 || ref.empty()) && heap.size() < kCapacity) {
                ts_cursor += rng() % 97; // non-decreasing, bursty ties
                Payload v{rng(), static_cast<std::uint32_t>(rng())};
                const auto hh = heap.push(ts_cursor, v);
                const auto wh = wheel.push(ts_cursor, v);
                ref.push(ts_cursor, next_ord, v);
                heap_h[hh.index] = hh;
                wheel_h[wh.index] = wh;
                ord_of_slot[hh.index] = next_ord++;
            } else if (roll < 80 || (heap.size() >= kCapacity && roll < 95)) {
                if (heap.empty())
                    continue;
                const Payload hp = heap.pop_min();
                const Payload wp = wheel.pop_min();
                const RefEvent want = ref.front();
                if (!(hp == want.value) || !(wp == want.value))
                    return std::fprintf(stderr, "mirror pop mismatch at step %llu\n",
                                        static_cast<unsigned long long>(step)),
                           1;
                for (std::size_t i = 0; i < kCapacity; ++i)
                    if (ord_of_slot[i] == want.ord) {
                        ord_of_slot[i] = ~std::uint64_t{0};
                        break;
                    }
                ref.pop();
                ++pops;
            } else if (roll < 95) {
                const std::uint32_t idx = static_cast<std::uint32_t>(rng() % kCapacity);
                const bool hg = heap.cancel(heap_h[idx]);
                const bool wg = wheel.cancel(wheel_h[idx]);
                const bool want =
                    ord_of_slot[idx] != ~std::uint64_t{0} && ref.cancel_ord(ord_of_slot[idx]);
                if (hg != want || wg != want)
                    return std::fprintf(stderr, "mirror cancel mismatch at step %llu\n",
                                        static_cast<unsigned long long>(step)),
                           1;
                if (hg) {
                    ord_of_slot[idx] = ~std::uint64_t{0};
                    ++cancels_ok;
                }
            } else if (!heap.empty()) {
                if (heap.peek_min_ts() != wheel.peek_min_ts() ||
                    !(heap.peek_min() == wheel.peek_min()))
                    return std::fprintf(stderr, "mirror peek mismatch at step %llu\n",
                                        static_cast<unsigned long long>(step)),
                           1;
            }
            if (step % 997 == 0 && (!heap.audit() || !wheel.audit()))
                return std::fprintf(stderr, "mirror audit failed at step %llu\n",
                                    static_cast<unsigned long long>(step)),
                       1;
        }
        if (heap.size() != ref.size() || wheel.size() != ref.size() || !heap.audit() ||
            !wheel.audit()) {
            std::fputs("mirror final state mismatch\n", stderr);
            return 1;
        }
        std::printf("cross-engine mirror OK (pops=%llu cancels=%llu)\n",
                    static_cast<unsigned long long>(pops),
                    static_cast<unsigned long long>(cancels_ok));
    }

    // Phase 3: determinism harness. Same scripted corpus, five fresh replays
    // per engine, every SHA-256 digest matching run one - and the two engines
    // agreeing with each other.
    {
        std::mt19937_64 gen(20260823 ^ 0x5eed);
        std::vector<OpRecord> corpus;
        corpus.reserve(300000);
        std::uint64_t ts_cursor = 0;
        for (std::size_t i = 0; i < 300000; ++i) {
            OpRecord op{};
            const int roll = static_cast<int>(gen() % 100);
            op.kind = roll < 60 ? 0 : roll < 85 ? 1 : 2;
            ts_cursor += gen() % 50; // non-decreasing: valid for both engines
            op.ts = ts_cursor;
            op.a = gen();
            op.b = static_cast<std::uint32_t>(gen());
            corpus.push_back(op);
        }
        const auto heap_digest = replay_and_hash<mog::Scheduler<Payload>>(corpus);
        for (int run = 1; run < 5; ++run)
            if (replay_and_hash<mog::Scheduler<Payload>>(corpus) != heap_digest) {
                std::fputs("FAIL heap determinism across runs\n", stderr);
                return 1;
            }
        const auto wheel_digest = replay_and_hash<mog::TimingWheel<Payload>>(corpus);
        for (int run = 1; run < 5; ++run)
            if (replay_and_hash<mog::TimingWheel<Payload>>(corpus) != wheel_digest) {
                std::fputs("FAIL wheel determinism across runs\n", stderr);
                return 1;
            }
        if (heap_digest != wheel_digest) {
            std::fputs("FAIL engines disagree on pop stream\n", stderr);
            return 1;
        }
        char scratch[65];
        const auto view = mog::Sha256::hex(heap_digest, scratch);
        std::fputs("sha256 trace: ", stdout);
        std::fwrite(view.data(), 1, view.size(), stdout);
        std::putchar('\n');
    }

    std::puts("scheduler tests OK");
    return 0;
}
