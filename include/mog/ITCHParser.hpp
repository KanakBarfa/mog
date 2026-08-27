// Zero-copy ITCH 5.0 stream parser: framing LUT, per-ISA kernel tables, Sink concept,
// and a stable trace hash for determinism checks.
#pragma once

#include <mog/Build.hpp>
#include <mog/Contracts.hpp>
#include <mog/IsaDispatch.hpp>
#include <mog/Types.hpp>
#include <mog/simd/SWAR.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace mog {

struct ParseError {
    DecodeError code;
    std::size_t offset;
    friend constexpr bool operator==(const ParseError&, const ParseError&) = default;
};

// Message length in bytes by type character; 0 means not part of the supported subset.
struct FrameTable {
    std::uint8_t length[256];
};

[[nodiscard]] constexpr FrameTable make_frame_table() noexcept {
    FrameTable t{};
    t.length[static_cast<unsigned char>('A')] = wire::kAddOrderSize;
    t.length[static_cast<unsigned char>('F')] = wire::kAddOrderAttributionSize;
    t.length[static_cast<unsigned char>('E')] = wire::kOrderExecutedSize;
    t.length[static_cast<unsigned char>('C')] = wire::kOrderExecutedWithPriceSize;
    t.length[static_cast<unsigned char>('X')] = wire::kOrderCancelSize;
    t.length[static_cast<unsigned char>('D')] = wire::kOrderDeleteSize;
    t.length[static_cast<unsigned char>('U')] = wire::kOrderReplaceSize;
    return t;
}

inline constexpr FrameTable kFrame = make_frame_table();

// Standard ITCH 5.0 message lengths beyond the decoding subset above. These
// are framed correctly and skipped by the session-tolerant path below instead
// of failing the stream: real captures interleave System Event, Trading
// Action, NOII, Trade prints and friends around the order-lifecycle types.
// Lengths follow the NASDAQ ITCH 5.0 specification.
[[nodiscard]] constexpr FrameTable make_session_frame_table() noexcept {
    FrameTable t = make_frame_table();
    t.length[static_cast<unsigned char>('S')] = 12; // System Event
    t.length[static_cast<unsigned char>('R')] = 39; // Stock Directory
    t.length[static_cast<unsigned char>('H')] = 25; // Stock Trading Action
    t.length[static_cast<unsigned char>('Y')] = 20; // Reg SHO Restriction
    t.length[static_cast<unsigned char>('L')] = 26; // Market Participant Position
    t.length[static_cast<unsigned char>('V')] = 35; // MWCB Decline Level
    t.length[static_cast<unsigned char>('W')] = 12; // MWCB Status
    t.length[static_cast<unsigned char>('K')] = 28; // IPO Quoting Period Update
    t.length[static_cast<unsigned char>('I')] = 50; // NOII
    t.length[static_cast<unsigned char>('N')] = 20; // RPII
    t.length[static_cast<unsigned char>('Q')] = 40; // Cross Trade
    t.length[static_cast<unsigned char>('P')] = 44; // Trade (Non-Cross)
    t.length[static_cast<unsigned char>('B')] = 19; // Broken Trade / Execution Break
    t.length[static_cast<unsigned char>('J')] = 35; // LULD Auction Collar
    t.length[static_cast<unsigned char>('O')] = 48; // Direct Listing Price Discovery
    t.length[static_cast<unsigned char>('h')] = 21; // Operational Halt
    return t;
}

inline constexpr FrameTable kSessionFrame = make_session_frame_table();

// Per-run accounting for the session-tolerant parser.
struct SessionStats {
    std::size_t decoded = 0;
    std::size_t skipped = 0;
    std::size_t skipped_by_type[256] = {};
};

template <typename S>
concept MessageSink = requires(S sink, const Message& message) {
    { sink.on_message(message) } -> std::same_as<void>;
};

namespace detail {

// Fast-path field loads straight from the stream; no packed-struct staging copy.
// Accept/reject semantics must stay byte-identical to the Types.hpp reference codecs.

[[nodiscard]] inline Header load_header(const unsigned char* p) noexcept {
    return Header{
        .locate = Locate{wire::load_be16(p + 1)},
        .tracking = Tracking{wire::load_be16(p + 3)},
        .ts_ns = TimestampNs{wire::load_be48(p + 5)},
    };
}

[[nodiscard]] inline Symbol load_symbol(const unsigned char* p) noexcept {
    Symbol sym;
    swar::store64(reinterpret_cast<unsigned char*>(sym.data()), swar::load64(p));
    return sym;
}

template <Isa>
[[nodiscard]] inline bool parse_one(const unsigned char* buf, std::size_t available,
                                    Message& out) noexcept {
    if (available < wire::kHeaderSize)
        return false;
    const auto type = static_cast<char>(buf[0]);
    const std::uint8_t need = kFrame.length[static_cast<unsigned char>(type)];
    if (need == 0 || available < need)
        return false;
    out.type = type;
    switch (type) {
    case 'A': {
        if (buf[19] != 'B' && buf[19] != 'S')
            return false;
        out.add_order = AddOrderMsg{
            .header = load_header(buf),
            .order_ref = OrderId{wire::load_be64(buf + 11)},
            .side = side_from_wire(static_cast<char>(buf[19])),
            .shares = Qty{static_cast<std::int64_t>(wire::load_be32(buf + 20))},
            .stock = load_symbol(buf + 24),
            .price = Price{static_cast<std::int64_t>(wire::load_be32(buf + 32))},
        };
        return true;
    }
    case 'F': {
        if (buf[19] != 'B' && buf[19] != 'S')
            return false;
        out.add_order_attribution = AddOrderAttributionMsg{
            .header = load_header(buf),
            .order_ref = OrderId{wire::load_be64(buf + 11)},
            .side = side_from_wire(static_cast<char>(buf[19])),
            .shares = Qty{static_cast<std::int64_t>(wire::load_be32(buf + 20))},
            .stock = load_symbol(buf + 24),
            .price = Price{static_cast<std::int64_t>(wire::load_be32(buf + 32))},
            .attribution = {static_cast<char>(buf[36]), static_cast<char>(buf[37]),
                            static_cast<char>(buf[38]), static_cast<char>(buf[39])},
        };
        return true;
    }
    case 'E': {
        out.order_executed = OrderExecutedMsg{
            .header = load_header(buf),
            .order_ref = OrderId{wire::load_be64(buf + 11)},
            .executed_shares = Qty{static_cast<std::int64_t>(wire::load_be32(buf + 19))},
            .match_number = MatchNumber{wire::load_be64(buf + 23)},
        };
        return true;
    }
    case 'C': {
        out.order_executed_with_price = OrderExecutedWithPriceMsg{
            .header = load_header(buf),
            .order_ref = OrderId{wire::load_be64(buf + 11)},
            .executed_shares = Qty{static_cast<std::int64_t>(wire::load_be32(buf + 19))},
            .match_number = MatchNumber{wire::load_be64(buf + 23)},
            .printable = buf[31] == 'Y',
            .execution_price = Price{static_cast<std::int64_t>(wire::load_be32(buf + 32))},
        };
        return true;
    }
    case 'X': {
        out.order_cancel = OrderCancelMsg{
            .header = load_header(buf),
            .order_ref = OrderId{wire::load_be64(buf + 11)},
            .cancelled_shares = Qty{static_cast<std::int64_t>(wire::load_be32(buf + 19))},
        };
        return true;
    }
    case 'D': {
        out.order_delete = OrderDeleteMsg{
            .header = load_header(buf),
            .order_ref = OrderId{wire::load_be64(buf + 11)},
        };
        return true;
    }
    case 'U': {
        out.order_replace = OrderReplaceMsg{
            .header = load_header(buf),
            .original_order_ref = OrderId{wire::load_be64(buf + 11)},
            .new_order_ref = OrderId{wire::load_be64(buf + 19)},
            .shares = Qty{static_cast<std::int64_t>(wire::load_be32(buf + 27))},
            .price = Price{static_cast<std::int64_t>(wire::load_be32(buf + 31))},
        };
        return true;
    }
    default:
        return false;
    }
}

constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

inline void hash_fold(std::uint64_t& h, std::uint64_t value) noexcept {
    h ^= value;
    h *= kFnvPrime;
}

inline void hash_fold_bytes(std::uint64_t& h, const char* data, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint8_t>(data[i]);
        h *= kFnvPrime;
    }
}

[[nodiscard]] inline DecodeError classify_stream_error(const unsigned char* msg,
                                                       std::size_t available) noexcept {
    if (available < wire::kHeaderSize)
        return DecodeError::truncated;
    const std::size_t need = kFrame.length[msg[0]];
    if (need == 0)
        return DecodeError::unknown_type;
    if (available < need)
        return DecodeError::truncated;
    if ((msg[0] == 'A' || msg[0] == 'F')) {
        const char side = static_cast<char>(msg[19]);
        if (side != 'B' && side != 'S')
            return DecodeError::invalid_side;
    }
    return DecodeError::truncated; // unreachable when parse_one agrees with this classifier
}

} // namespace detail

// One function pointer per selected kernel; built once per process.
struct KernelTable {
    bool (*parse_one)(const unsigned char*, std::size_t, Message&) noexcept;
};

namespace detail {

template <Isa I>
bool parse_one_thunk(const unsigned char* buf, std::size_t available, Message& out) noexcept {
    return parse_one<I>(buf, available, out);
}

} // namespace detail

[[nodiscard]] inline KernelTable kernels_for(Isa isa) noexcept {
    switch (isa) {
    case Isa::avx512:
        return KernelTable{&detail::parse_one_thunk<Isa::avx512>};
    case Isa::avx2:
        return KernelTable{&detail::parse_one_thunk<Isa::avx2>};
    case Isa::sse4_baseline:
        break;
    }
    return KernelTable{&detail::parse_one_thunk<Isa::sse4_baseline>};
}

// Process-wide table chosen by CPUID at first use.
[[nodiscard]] inline const KernelTable& active_kernels() noexcept {
    static const KernelTable table = kernels_for(detect_isa());
    return table;
}

// Stable FNV-1a over decoded fields, in fixed per-type order; the backbone of
// determinism checks and golden files until the M3 trace hash lands.
[[nodiscard]] inline std::uint64_t trace_hash(const Message& m,
                                              std::uint64_t seed = 0xcbf29ce484222325ULL) noexcept {
    std::uint64_t h = seed;
    detail::hash_fold(h, static_cast<std::uint8_t>(m.type));
    switch (m.type) {
    case 'A': {
        const auto& v = m.add_order;
        detail::hash_fold(h, v.header.locate.value);
        detail::hash_fold(h, v.header.tracking.value);
        detail::hash_fold(h, v.header.ts_ns.value);
        detail::hash_fold(h, v.order_ref.value);
        detail::hash_fold(h, static_cast<std::uint8_t>(v.side));
        detail::hash_fold(h, static_cast<std::uint64_t>(v.shares.units));
        detail::hash_fold_bytes(h, v.stock.data(), 8);
        detail::hash_fold(h, static_cast<std::uint64_t>(v.price.ticks));
        break;
    }
    case 'F': {
        const auto& v = m.add_order_attribution;
        detail::hash_fold(h, v.header.locate.value);
        detail::hash_fold(h, v.header.tracking.value);
        detail::hash_fold(h, v.header.ts_ns.value);
        detail::hash_fold(h, v.order_ref.value);
        detail::hash_fold(h, static_cast<std::uint8_t>(v.side));
        detail::hash_fold(h, static_cast<std::uint64_t>(v.shares.units));
        detail::hash_fold_bytes(h, v.stock.data(), 8);
        detail::hash_fold(h, static_cast<std::uint64_t>(v.price.ticks));
        detail::hash_fold_bytes(h, v.attribution.data(), 4);
        break;
    }
    case 'E': {
        const auto& v = m.order_executed;
        detail::hash_fold(h, v.header.locate.value);
        detail::hash_fold(h, v.header.tracking.value);
        detail::hash_fold(h, v.header.ts_ns.value);
        detail::hash_fold(h, v.order_ref.value);
        detail::hash_fold(h, static_cast<std::uint64_t>(v.executed_shares.units));
        detail::hash_fold(h, v.match_number.value);
        break;
    }
    case 'C': {
        const auto& v = m.order_executed_with_price;
        detail::hash_fold(h, v.header.locate.value);
        detail::hash_fold(h, v.header.tracking.value);
        detail::hash_fold(h, v.header.ts_ns.value);
        detail::hash_fold(h, v.order_ref.value);
        detail::hash_fold(h, static_cast<std::uint64_t>(v.executed_shares.units));
        detail::hash_fold(h, v.match_number.value);
        detail::hash_fold(h, v.printable ? 1 : 0);
        detail::hash_fold(h, static_cast<std::uint64_t>(v.execution_price.ticks));
        break;
    }
    case 'X': {
        const auto& v = m.order_cancel;
        detail::hash_fold(h, v.header.locate.value);
        detail::hash_fold(h, v.header.tracking.value);
        detail::hash_fold(h, v.header.ts_ns.value);
        detail::hash_fold(h, v.order_ref.value);
        detail::hash_fold(h, static_cast<std::uint64_t>(v.cancelled_shares.units));
        break;
    }
    case 'D': {
        const auto& v = m.order_delete;
        detail::hash_fold(h, v.header.locate.value);
        detail::hash_fold(h, v.header.tracking.value);
        detail::hash_fold(h, v.header.ts_ns.value);
        detail::hash_fold(h, v.order_ref.value);
        break;
    }
    case 'U': {
        const auto& v = m.order_replace;
        detail::hash_fold(h, v.header.locate.value);
        detail::hash_fold(h, v.header.tracking.value);
        detail::hash_fold(h, v.header.ts_ns.value);
        detail::hash_fold(h, v.original_order_ref.value);
        detail::hash_fold(h, v.new_order_ref.value);
        detail::hash_fold(h, static_cast<std::uint64_t>(v.shares.units));
        detail::hash_fold(h, static_cast<std::uint64_t>(v.price.ticks));
        break;
    }
    default:
        break;
    }
    return h;
}

// Parses [buf, buf+len) invoking sink.on_message for every event using the
template <Isa I, MessageSink Sink>
[[nodiscard]] std::expected<std::size_t, ParseError>
parse_itch_isa(const unsigned char* buf, std::size_t len, Sink& sink) noexcept {
    std::size_t off = 0;
    Message msg{};
    while (off < len) {
        if (!detail::parse_one<I>(buf + off, len - off, msg)) {
            return std::unexpected(
                ParseError{detail::classify_stream_error(buf + off, len - off), off});
        }
        sink.on_message(msg);
        const std::size_t msg_len = kFrame.length[static_cast<unsigned char>(msg.type)];
        if (msg_len == 0)
            return std::unexpected(ParseError{DecodeError::unknown_type, off});
        off += msg_len;
    }
    return off;
}

// CPUID-selected kernel table with inlined static dispatch. Returns bytes consumed or error.
template <MessageSink Sink>
[[nodiscard]] std::expected<std::size_t, ParseError>
parse_itch(const unsigned char* buf, std::size_t len, Sink& sink) noexcept {
    switch (detect_isa()) {
    case Isa::avx512:
        return parse_itch_isa<Isa::avx512>(buf, len, sink);
    case Isa::avx2:
        return parse_itch_isa<Isa::avx2>(buf, len, sink);
    default:
        return parse_itch_isa<Isa::sse4_baseline>(buf, len, sink);
    }
}

// Forced-variant overload; differential tests and benchmarks pin the ISA.
template <MessageSink Sink>
[[nodiscard]] std::expected<std::size_t, ParseError>
parse_itch_as(const unsigned char* buf, std::size_t len, Sink& sink,
              const KernelTable& kernels) noexcept {
    std::size_t off = 0;
    Message msg{};
    while (off < len) {
        if (!kernels.parse_one(buf + off, len - off, msg)) {
            return std::unexpected(
                ParseError{detail::classify_stream_error(buf + off, len - off), off});
        }
        sink.on_message(msg);
        const std::size_t msg_len = kFrame.length[static_cast<unsigned char>(msg.type)];
        if (msg_len == 0)
            return std::unexpected(ParseError{DecodeError::unknown_type, off});
        off += msg_len;
    }
    return off;
}

template <Isa I, MessageSink Sink>
[[nodiscard]] std::expected<SessionStats, ParseError>
parse_itch_session_isa(const unsigned char* buf, std::size_t len, Sink& sink) noexcept {
    SessionStats stats;
    std::size_t off = 0;
    Message msg{};
    while (off < len) {
        const unsigned char type = buf[off];
        const std::size_t need = kSessionFrame.length[type];
        if (need == 0)
            return std::unexpected(ParseError{DecodeError::unknown_type, off});
        if (len - off < need)
            return std::unexpected(ParseError{DecodeError::truncated, off});
        if (kFrame.length[type] == 0) {
            ++stats.skipped;
            ++stats.skipped_by_type[type];
            off += need;
            continue;
        }
        if (!detail::parse_one<I>(buf + off, len - off, msg)) {
            return std::unexpected(
                ParseError{detail::classify_stream_error(buf + off, len - off), off});
        }
        sink.on_message(msg);
        ++stats.decoded;
        off += need;
    }
    return stats;
}

// Session-tolerant replay path: decodes the order-lifecycle subset exactly as
// parse_itch_as does (same kernels, same accept/reject semantics), but frames
// and skips the remaining standard ITCH 5.0 types instead of failing the
// stream. Skips are counted per type in the returned stats - tolerance here is
// explicit and auditable, never silent. Types unknown to BOTH tables remain
// loud errors.
template <MessageSink Sink>
[[nodiscard]] std::expected<SessionStats, ParseError>
parse_itch_session_as(const unsigned char* buf, std::size_t len, Sink& sink,
                      const KernelTable& kernels) noexcept {
    SessionStats stats;
    std::size_t off = 0;
    Message msg{};
    while (off < len) {
        const unsigned char type = buf[off];
        const std::size_t need = kSessionFrame.length[type];
        if (need == 0)
            return std::unexpected(ParseError{DecodeError::unknown_type, off});
        if (len - off < need)
            return std::unexpected(ParseError{DecodeError::truncated, off});
        if (kFrame.length[type] == 0) {
            ++stats.skipped;
            ++stats.skipped_by_type[type];
            off += need;
            continue;
        }
        if (!kernels.parse_one(buf + off, len - off, msg)) {
            // Frame length already validated above; a kernel rejection here is
            // a field-level fault (e.g. invalid side), classified as such.
            return std::unexpected(
                ParseError{detail::classify_stream_error(buf + off, len - off), off});
        }
        sink.on_message(msg);
        ++stats.decoded;
        off += need;
    }
    return stats;
}

template <MessageSink Sink>
[[nodiscard]] std::expected<SessionStats, ParseError>
parse_itch_session(const unsigned char* buf, std::size_t len, Sink& sink) noexcept {
    switch (detect_isa()) {
    case Isa::avx512:
        return parse_itch_session_isa<Isa::avx512>(buf, len, sink);
    case Isa::avx2:
        return parse_itch_session_isa<Isa::avx2>(buf, len, sink);
    default:
        return parse_itch_session_isa<Isa::sse4_baseline>(buf, len, sink);
    }
}

template <Isa SelectedIsa = Isa::sse4_baseline, MessageSink Sink, typename SkipListener>
    requires requires(SkipListener l, char t, const unsigned char* p, std::size_t n) { l(t, p, n); }
[[nodiscard]] std::expected<SessionStats, ParseError>
parse_itch_session_listen_isa(const unsigned char* buf, std::size_t len, Sink& sink,
                              SkipListener& listen) noexcept {
    SessionStats stats;
    std::size_t off = 0;
    Message msg{};
    while (off < len) {
        const unsigned char type = buf[off];
        const std::size_t need = kSessionFrame.length[type];
        if (need == 0)
            return std::unexpected(ParseError{DecodeError::unknown_type, off});
        if (len - off < need)
            return std::unexpected(ParseError{DecodeError::truncated, off});
        if (kFrame.length[type] == 0) {
            ++stats.skipped;
            ++stats.skipped_by_type[type];
            listen(static_cast<char>(type), buf + off, need);
            off += need;
            continue;
        }
        if (!detail::parse_one<SelectedIsa>(buf + off, len - off, msg)) {
            return std::unexpected(
                ParseError{detail::classify_stream_error(buf + off, len - off), off});
        }
        sink.on_message(msg);
        ++stats.decoded;
        off += need;
    }
    return stats;
}

template <MessageSink Sink, typename SkipListener>
    requires requires(SkipListener l, char t, const unsigned char* p, std::size_t n) { l(t, p, n); }
[[nodiscard]] inline std::expected<SessionStats, ParseError>
parse_itch_session_listen(const unsigned char* buf, std::size_t len, Sink& sink,
                          SkipListener& listen) noexcept {
    switch (detect_isa()) {
    case Isa::avx512:
        return parse_itch_session_listen_isa<Isa::avx512>(buf, len, sink, listen);
    case Isa::avx2:
        return parse_itch_session_listen_isa<Isa::avx2>(buf, len, sink, listen);
    default:
        return parse_itch_session_listen_isa<Isa::sse4_baseline>(buf, len, sink, listen);
    }
}

// Optional visibility into skipped standard frames without decoding them into
// the Message union: listeners receive (type char, frame ptr, frame length)
// for every type outside the core subset. This is how session-level modeling
// (System Event, Trading Action, NOII, Reg SHO) observes the stream while the
// strict decoder stays untouched.
template <MessageSink Sink, typename SkipListener>
    requires requires(SkipListener l, char t, const unsigned char* p, std::size_t n) { l(t, p, n); }
[[nodiscard]] inline std::expected<SessionStats, ParseError>
parse_itch_session_listen_as(const unsigned char* buf, std::size_t len, Sink& sink,
                             const KernelTable&, SkipListener& listen) noexcept {
    return parse_itch_session_listen(buf, len, sink, listen);
}

} // namespace mog
