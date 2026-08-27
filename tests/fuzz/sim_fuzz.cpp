// Fuzzer: byte-driven scripts against the execution simulator.
// Properties: no crash; audit() holds after each settled step; and the
// core product claim itself - the same script yields the identical trace
// digest across two fresh runs.
#include <mog/Simulate.hpp>

#include <cstddef>
#include <cstdint>

namespace {

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v;
    h *= 0x100000001b3ull;
    return h ^ (h >> 29);
}

struct Cursor {
    const std::uint8_t* d;
    std::size_t n;
    std::size_t off = 0;
    [[nodiscard]] std::uint8_t u8() {
        if (off >= n)
            return 0;
        return d[off++];
    }
    [[nodiscard]] std::uint64_t u16() { return (std::uint64_t(u8()) << 8) | u8(); }
};

void run_script(const std::uint8_t* data, std::size_t size, std::uint64_t& out) {
    using namespace mog;

    SimConfig cfg{};
    cfg.book.arena_capacity = 4096;
    cfg.book.ladder = {0, 4000, 64};
    cfg.event_capacity = 4096;
    cfg.jitter_kind = JitterKind::none;
    ExecutionSimulator sim(cfg);

    Cursor c{data, size};
    const std::uint64_t strat_base = (std::uint64_t{1} << 62) + 1000;
    std::uint64_t now = 0;
    std::uint64_t next_ref = strat_base;

    while (c.off < c.n) {
        const std::uint64_t op = c.u8() & 7;
        switch (op) {
        case 0: { // seed external
            const auto side = (c.u8() & 1) != 0 ? Side::sell : Side::buy;
            const Qty qty{static_cast<std::int64_t>(c.u16() % 500) + 1};
            const Price px{static_cast<std::int64_t>(c.u16() % 4001)};
            static_cast<void>(sim.seed_external(OrderId{c.u16()}, side, qty, px));
            break;
        }
        case 1: { // strategy limit order
            const auto side = (c.u8() & 1) != 0 ? Side::sell : Side::buy;
            const Qty qty{static_cast<std::int64_t>(c.u8()) + 1};
            const Price px{static_cast<std::int64_t>(c.u16() % 4001)};
            SimInbound o{};
            o.ref = OrderId{next_ref++};
            o.side = side;
            o.qty = qty;
            o.price = px;
            static_cast<void>(sim.submit(o, now + c.u8()));
            break;
        }
        case 2: { // external pressure on one resting side
            const auto side = (c.u8() & 1) != 0 ? Side::sell : Side::buy;
            sim.apply_external(side, Price{static_cast<std::int64_t>(c.u16() % 4001)},
                               static_cast<std::int64_t>(c.u8()));
            break;
        }
        case 3: // time passes
            now += c.u8();
            sim.advance_time(c.u8());
            break;
        case 4: // cancel a strategy ref from a small recent window
            if (next_ref > strat_base)
                static_cast<void>(sim.cancel_strategy(OrderId{strat_base + (c.u8() % 32)}));
            break;
        default: // drain pending work before continuing
            sim.drain();
            break;
        }
    }
    sim.drain();

    if (!sim.audit()) [[unlikely]]
        __builtin_trap();
    for (const auto& f : sim.reports())
        if (f.qty <= 0 || f.price_ticks < 0 || f.price_ticks > 4000) [[unlikely]]
            __builtin_trap();

    out = mix(0xcbf29ce484222325ull, next_ref);
    for (const auto& f : sim.reports())
        out = mix(out, mix(f.ref, static_cast<std::uint64_t>(f.price_ticks)) ^
                           (static_cast<std::uint64_t>(f.qty) << 1));
    char hex[65];
    const auto dg = Sha256::hex(sim.trace_digest(), hex);
    for (const char i : dg)
        out = mix(out, static_cast<unsigned char>(i));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // The product claim under fuzz: identical script, identical digest.
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    run_script(data, size, a);
    run_script(data, size, b);
    if (a != b) [[unlikely]]
        __builtin_trap();
    return 0;
}
