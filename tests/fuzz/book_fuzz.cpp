// Fuzzer: byte-driven operation scripts against the L3 book.
// Bytes decode to add/cancel/execute/replace ops over a small ref pool so
// collisions, unknown refs, and partial fills happen constantly.
// Properties: no crash; audit() holds after every op; the whole script is
// deterministic (two passes produce identical op-result hashes).
#include <mog/OrderBook.hpp>

#include <cstddef>
#include <cstdint>
#include <unistd.h>

namespace {

std::uint64_t g_digest = 0;

std::uint64_t mix(std::uint64_t h, const mog::BookTick& t) {
    std::uint64_t x = h;
    const auto* p = reinterpret_cast<const unsigned char*>(&t);
    for (std::size_t i = 0; i < sizeof(t); ++i) {
        x ^= p[i];
        x *= 0x100000001b3ull;
    }
    return x;
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
    [[nodiscard]] std::uint64_t u16() {
        const std::uint64_t v = (std::uint64_t(u8()) << 8) | u8();
        return v;
    }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace mog;

    for (int pass = 0; pass < 2; ++pass) {
        OrderBook<>::Config bc{};
        bc.arena_capacity = 4096;
        bc.ladder = {0, 4000, 64};
        OrderBook<> book(bc);
        Cursor c{data, size};
        std::uint64_t h = 0xcbf29ce484222325ull;

        while (c.off < c.n) {
            const std::uint64_t op = c.u8() & 3;
            const std::uint64_t ref = c.u16();
            switch (op) {
            case 0: { // add
                const auto side = (c.u8() & 1) != 0 ? Side::sell : Side::buy;
                const Qty qty{static_cast<std::int64_t>(c.u8()) + 1};
                const Price px{static_cast<std::int64_t>(c.u16() % 4001)};
                h = mix(h, book.add(OrderId{ref}, side, qty, px));
                break;
            }
            case 1: // cancel
                h = mix(h, book.cancel(OrderId{ref}, Qty{static_cast<std::int64_t>(c.u8())}));
                break;
            case 2: // execute
                h = mix(h, book.execute(OrderId{ref}, Qty{static_cast<std::int64_t>(c.u8())}));
                break;
            default: { // replace
                const Qty qty{static_cast<std::int64_t>(c.u8()) + 1};
                const Price px{static_cast<std::int64_t>(c.u16() % 4001)};
                h = mix(h, book.replace(OrderId{ref}, OrderId{ref + 4096}, qty, px));
                break;
            }
            }
            if (!book.audit()) [[unlikely]] {
                (void)!::write(1, "AUDIT\n", 6);
                __builtin_trap();
            }
        }

        if (pass == 0) {
            g_digest = h;
        } else if (h != g_digest) [[unlikely]] {
            (void)!::write(1, "NONDET\n", 7);
            __builtin_trap(); // same input, different behavior: nondeterminism
        }
    }
    return 0;
}
