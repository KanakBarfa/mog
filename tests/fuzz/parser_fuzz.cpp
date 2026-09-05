// Fuzzer: raw bytes through the strict and session-tolerant parser paths.
// Property 1: never crashes; consumed <= len; error offsets stay in range.
// Property 2 (differential): when the strict path accepts a stream, the
// session-tolerant path must decode the exact same messages (same count,
// same fields). A divergence there is a framing/kernel inconsistency.
#include <mog/ITCHParser.hpp>

#include <cstddef>
#include <cstdint>

namespace {

constexpr std::uint64_t kFnv = 0x100000001b3ull;

std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept {
    h ^= v;
    h *= kFnv;
    return h;
}

// Field-wise message digest; Message carries padding and an inactive union
// arm, so raw byte hashing is undefined-comparison territory.
struct HashSink {
    std::uint64_t h = 0xcbf29ce484222325ull;
    std::size_t n = 0;

    void on_message(const mog::Message& m) {
        ++n;
        const mog::Header& hdr = m.type == 'A'   ? m.add_order.header
                                 : m.type == 'F' ? m.add_order_attribution.header
                                 : m.type == 'E' ? m.order_executed.header
                                 : m.type == 'C' ? m.order_executed_with_price.header
                                 : m.type == 'X' ? m.order_cancel.header
                                 : m.type == 'D' ? m.order_delete.header
                                                 : m.order_replace.header;
        h = mix(h, static_cast<unsigned char>(m.type));
        h = mix(h, hdr.locate.value);
        h = mix(h, hdr.tracking.value);
        h = mix(h, hdr.ts_ns.value);
        switch (m.type) {
        case 'A':
            h = mix(h, m.add_order.order_ref.value);
            h = mix(h, m.add_order.shares.units);
            h = mix(h, m.add_order.price.ticks);
            h = mix(h, mog::to_wire(m.add_order.side));
            for (const char c : m.add_order.stock)
                h = mix(h, static_cast<unsigned char>(c));
            break;
        case 'F':
            h = mix(h, m.add_order_attribution.order_ref.value);
            h = mix(h, m.add_order_attribution.shares.units);
            h = mix(h, m.add_order_attribution.price.ticks);
            h = mix(h, mog::to_wire(m.add_order_attribution.side));
            for (const char c : m.add_order_attribution.stock)
                h = mix(h, static_cast<unsigned char>(c));
            for (const char ch : m.add_order_attribution.attribution)
                h = mix(h, static_cast<unsigned char>(ch));
            break;
        case 'E':
            h = mix(h, m.order_executed.order_ref.value);
            h = mix(h, m.order_executed.executed_shares.units);
            h = mix(h, m.order_executed.match_number.value);
            break;
        case 'C':
            h = mix(h, m.order_executed_with_price.order_ref.value);
            h = mix(h, m.order_executed_with_price.executed_shares.units);
            h = mix(h, m.order_executed_with_price.match_number.value);
            h = mix(h, m.order_executed_with_price.execution_price.ticks);
            h = mix(h, m.order_executed_with_price.printable);
            break;
        case 'X':
            h = mix(h, m.order_cancel.order_ref.value);
            h = mix(h, m.order_cancel.cancelled_shares.units);
            break;
        case 'D':
            h = mix(h, m.order_delete.order_ref.value);
            break;
        default:
            h = mix(h, m.order_replace.original_order_ref.value);
            h = mix(h, m.order_replace.new_order_ref.value);
            h = mix(h, m.order_replace.shares.units);
            h = mix(h, m.order_replace.price.ticks);
            break;
        }
    }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace mog;

    HashSink strict;
    const auto rs = parse_itch(data, size, strict);
    if (rs.has_value()) {
        if (*rs > size) [[unlikely]]
            __builtin_trap();
    } else if (rs.error().offset >= size) [[unlikely]] {
        __builtin_trap();
    }

    // Session-tolerant walk never fails where the strict walk succeeded:
    // strict acceptance implies every frame type is in the shared kernel
    // table and every field passed validation once already.
    HashSink session;
    const auto rr = parse_itch_session_as(data, size, session, active_kernels());
    if (rs.has_value()) {
        if (!rr.has_value()) [[unlikely]]
            __builtin_trap();
        if (session.n != strict.n || session.h != strict.h) [[unlikely]]
            __builtin_trap();
    }
    return 0;
}
