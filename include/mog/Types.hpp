// Fixed-point Price/Qty with saturating public arithmetic, strong ids,
// and the ITCH 5.0 subset (A F E C X D U) wire structs with layout audits
// plus scalar reference encode/decode codecs.
#pragma once

#include <mog/Build.hpp>
#include <mog/Contracts.hpp>
#include <mog/Wire.hpp>
#include <mog/polyfill/Saturating.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

static_assert(MOG_HAS_EXPECTED, "std::expected is part of the C++23 consumer floor");

namespace mog {

inline constexpr std::int64_t kPriceScale = 10'000; // ITCH implied decimal 1/10000 USD
inline constexpr std::int64_t kValueLimit = std::int64_t{1} << 56;

template <class Tag, class T>
struct Wrapped {
    T value{};
    constexpr Wrapped() noexcept = default;
    constexpr explicit Wrapped(T v) noexcept : value(v) {}
    friend constexpr bool operator==(const Wrapped&, const Wrapped&) = default;
    friend constexpr auto operator<=>(const Wrapped&, const Wrapped&) = default;
};

struct OrderIdTag;
struct MatchIdTag;
struct LocateTag;
struct TrackingTag;
struct TimestampTag;
struct PriceTag;
struct QtyTag;

using OrderId = Wrapped<OrderIdTag, std::uint64_t>;
using MatchNumber = Wrapped<MatchIdTag, std::uint64_t>;
using Locate = Wrapped<LocateTag, std::uint16_t>;
using Tracking = Wrapped<TrackingTag, std::uint16_t>;
using TimestampNs = Wrapped<TimestampTag, std::uint64_t>;

enum class Side : char { buy = 'B', sell = 'S' };

constexpr char to_wire(Side s) noexcept {
    return static_cast<char>(s);
}
constexpr Side side_from_wire(char c) noexcept {
    return c == 'B' ? Side::buy : c == 'S' ? Side::sell : static_cast<Side>('?');
}

struct Price {
    std::int64_t ticks{};

    constexpr Price() noexcept = default;
    constexpr explicit Price(std::int64_t t) noexcept : ticks(t) {}

    friend constexpr bool operator==(const Price&, const Price&) = default;
    friend constexpr auto operator<=>(const Price&, const Price&) = default;

    // Public API is saturating (P0543 semantics); raw_* are hot-path unchecked
    // variants whose callers must uphold |operands| <= kValueLimit.
    [[nodiscard]] constexpr Price add(Price rhs) const noexcept {
        return Price{sat::add_sat(ticks, rhs.ticks)};
    }
    [[nodiscard]] constexpr Price sub(Price rhs) const noexcept {
        return Price{sat::sub_sat(ticks, rhs.ticks)};
    }
    [[nodiscard]] constexpr Price mul(std::int64_t factor) const noexcept {
        return Price{sat::mul_sat(ticks, factor)};
    }

    [[nodiscard]] constexpr Price raw_add(Price rhs) const noexcept {
        MOG_PRE(ticks <= kValueLimit && rhs.ticks <= kValueLimit);
        MOG_PRE(ticks >= -kValueLimit && rhs.ticks >= -kValueLimit);
        return Price{ticks + rhs.ticks};
    }
    [[nodiscard]] constexpr Price raw_sub(Price rhs) const noexcept {
        MOG_PRE(ticks <= kValueLimit && rhs.ticks <= kValueLimit);
        MOG_PRE(ticks >= -kValueLimit && rhs.ticks >= -kValueLimit);
        return Price{ticks - rhs.ticks};
    }

    [[nodiscard]] static constexpr Price zero() noexcept { return Price{}; }
};

struct Qty {
    std::int64_t units{};

    constexpr Qty() noexcept = default;
    constexpr explicit Qty(std::int64_t u) noexcept : units(u) {}

    friend constexpr bool operator==(const Qty&, const Qty&) = default;
    friend constexpr auto operator<=>(const Qty&, const Qty&) = default;

    [[nodiscard]] constexpr Qty add(Qty rhs) const noexcept {
        return Qty{sat::add_sat(units, rhs.units)};
    }
    [[nodiscard]] constexpr Qty sub(Qty rhs) const noexcept {
        return Qty{sat::sub_sat(units, rhs.units)};
    }

    // Raw decrement; callers guarantee non-negative results (book invariant upstream).
    [[nodiscard]] constexpr Qty raw_sub_nonneg(Qty rhs) const noexcept {
        MOG_PRE(rhs.units >= 0);
        MOG_PRE(units >= rhs.units);
        return Qty{units - rhs.units};
    }

    [[nodiscard]] static constexpr Qty zero() noexcept { return Qty{}; }
};

using Symbol = std::array<char, 8>;

enum class DecodeError : std::uint8_t {
    truncated = 1,
    unknown_type = 2,
    invalid_side = 3,
    invalid_symbol = 4,
};

[[nodiscard]] constexpr std::string_view error_text(DecodeError e) noexcept {
    switch (e) {
    case DecodeError::truncated:
        return "truncated";
    case DecodeError::unknown_type:
        return "unknown_type";
    case DecodeError::invalid_side:
        return "invalid_side";
    case DecodeError::invalid_symbol:
        return "invalid_symbol";
    }
    return "?";
}

namespace wire {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

struct __attribute__((packed)) Header {
    char type;
    std::uint16_t locate_be;
    std::uint16_t tracking_be;
    unsigned char ts_be[6];
};
struct __attribute__((packed)) AddOrderBody {
    Header header;
    std::uint64_t order_ref_be;
    char buy_sell;
    std::uint32_t shares_be;
    char stock[8];
    std::uint32_t price_be;
};
struct __attribute__((packed)) AddOrderAttributionBody {
    AddOrderBody base;
    char attribution[4];
};
struct __attribute__((packed)) OrderExecutedBody {
    Header header;
    std::uint64_t order_ref_be;
    std::uint32_t executed_shares_be;
    std::uint64_t match_number_be;
};
struct __attribute__((packed)) OrderExecutedWithPriceBody {
    Header header;
    std::uint64_t order_ref_be;
    std::uint32_t executed_shares_be;
    std::uint64_t match_number_be;
    char printable;
    std::uint32_t execution_price_be;
};
struct __attribute__((packed)) OrderCancelBody {
    Header header;
    std::uint64_t order_ref_be;
    std::uint32_t cancelled_shares_be;
};
struct __attribute__((packed)) OrderDeleteBody {
    Header header;
    std::uint64_t order_ref_be;
};
struct __attribute__((packed)) OrderReplaceBody {
    Header header;
    std::uint64_t original_order_ref_be;
    std::uint64_t new_order_ref_be;
    std::uint32_t shares_be;
    std::uint32_t price_be;
};

#pragma GCC diagnostic pop

inline constexpr std::size_t kHeaderSize = 11;
inline constexpr std::size_t kAddOrderSize = 36;
inline constexpr std::size_t kAddOrderAttributionSize = 40;
inline constexpr std::size_t kOrderExecutedSize = 31;
inline constexpr std::size_t kOrderExecutedWithPriceSize = 36;
inline constexpr std::size_t kOrderCancelSize = 23;
inline constexpr std::size_t kOrderDeleteSize = 19;
inline constexpr std::size_t kOrderReplaceSize = 35;
inline constexpr std::size_t kMaxMessageSize = 40;

static_assert(sizeof(Header) == kHeaderSize);
static_assert(sizeof(AddOrderBody) == kAddOrderSize);
static_assert(sizeof(AddOrderAttributionBody) == kAddOrderAttributionSize);
static_assert(sizeof(OrderExecutedBody) == kOrderExecutedSize);
static_assert(sizeof(OrderExecutedWithPriceBody) == kOrderExecutedWithPriceSize);
static_assert(sizeof(OrderCancelBody) == kOrderCancelSize);
static_assert(sizeof(OrderDeleteBody) == kOrderDeleteSize);
static_assert(sizeof(OrderReplaceBody) == kOrderReplaceSize);

static_assert(offsetof(Header, type) == 0);
static_assert(offsetof(Header, locate_be) == 1);
static_assert(offsetof(Header, tracking_be) == 3);
static_assert(offsetof(Header, ts_be) == 5);

static_assert(offsetof(AddOrderBody, order_ref_be) == 11);
static_assert(offsetof(AddOrderBody, buy_sell) == 19);
static_assert(offsetof(AddOrderBody, shares_be) == 20);
static_assert(offsetof(AddOrderBody, stock) == 24);
static_assert(offsetof(AddOrderBody, price_be) == 32);

static_assert(offsetof(AddOrderAttributionBody, attribution) == 36);

static_assert(offsetof(OrderExecutedBody, order_ref_be) == 11);
static_assert(offsetof(OrderExecutedBody, executed_shares_be) == 19);
static_assert(offsetof(OrderExecutedBody, match_number_be) == 23);

static_assert(offsetof(OrderExecutedWithPriceBody, printable) == 31);
static_assert(offsetof(OrderExecutedWithPriceBody, execution_price_be) == 32);

static_assert(offsetof(OrderCancelBody, cancelled_shares_be) == 19);
static_assert(offsetof(OrderDeleteBody, order_ref_be) == 11);

static_assert(offsetof(OrderReplaceBody, original_order_ref_be) == 11);
static_assert(offsetof(OrderReplaceBody, new_order_ref_be) == 19);
static_assert(offsetof(OrderReplaceBody, shares_be) == 27);
static_assert(offsetof(OrderReplaceBody, price_be) == 31);

} // namespace wire

struct Header {
    Locate locate{};
    Tracking tracking{};
    TimestampNs ts_ns{};
    friend constexpr bool operator==(const Header&, const Header&) = default;
};

struct AddOrderMsg {
    Header header;
    OrderId order_ref;
    Side side;
    Qty shares;
    Symbol stock{};
    Price price;
    friend constexpr bool operator==(const AddOrderMsg&, const AddOrderMsg&) = default;
};

struct AddOrderAttributionMsg {
    Header header;
    OrderId order_ref;
    Side side;
    Qty shares;
    Symbol stock{};
    Price price;
    std::array<char, 4> attribution{};
    friend constexpr bool operator==(const AddOrderAttributionMsg&,
                                     const AddOrderAttributionMsg&) = default;
};

struct OrderExecutedMsg {
    Header header;
    OrderId order_ref;
    Qty executed_shares;
    MatchNumber match_number;
    friend constexpr bool operator==(const OrderExecutedMsg&, const OrderExecutedMsg&) = default;
};

struct OrderExecutedWithPriceMsg {
    Header header;
    OrderId order_ref;
    Qty executed_shares;
    MatchNumber match_number;
    bool printable;
    Price execution_price;
    friend constexpr bool operator==(const OrderExecutedWithPriceMsg&,
                                     const OrderExecutedWithPriceMsg&) = default;
};

struct OrderCancelMsg {
    Header header;
    OrderId order_ref;
    Qty cancelled_shares;
    friend constexpr bool operator==(const OrderCancelMsg&, const OrderCancelMsg&) = default;
};

struct OrderDeleteMsg {
    Header header;
    OrderId order_ref;
    friend constexpr bool operator==(const OrderDeleteMsg&, const OrderDeleteMsg&) = default;
};

struct OrderReplaceMsg {
    Header header;
    OrderId original_order_ref;
    OrderId new_order_ref;
    Qty shares;
    Price price;
    friend constexpr bool operator==(const OrderReplaceMsg&, const OrderReplaceMsg&) = default;
};

struct Message {
    Message() noexcept : type{}, add_order_attribution{} {}
    char type{};
    // Only the member matching type is active; compare payloads field-wise, never whole.
    union {
        AddOrderMsg add_order;
        AddOrderAttributionMsg add_order_attribution;
        OrderExecutedMsg order_executed;
        OrderExecutedWithPriceMsg order_executed_with_price;
        OrderCancelMsg order_cancel;
        OrderDeleteMsg order_delete;
        OrderReplaceMsg order_replace;
    };
};

[[nodiscard]] constexpr std::string_view symbol_view(const Symbol& sym) noexcept {
    std::size_t len = 8;
    while (len > 0 && (sym[len - 1] == ' ' || sym[len - 1] == '\0'))
        --len;
    return std::string_view(sym.data(), len);
}

[[nodiscard]] constexpr Symbol symbol_from(std::string_view text) noexcept {
    MOG_PRE(text.size() <= 8);
    const std::size_t n = text.size() < 8 ? text.size() : 8;
    Symbol sym{};
    for (std::size_t i = 0; i < n; ++i)
        sym[i] = text[i];
    for (std::size_t i = n; i < 8; ++i)
        sym[i] = ' ';
    return sym;
}

namespace detail {

inline Header decode_header(const wire::Header& h) noexcept {
    return Header{
        .locate = Locate{wire::swap16(h.locate_be)},
        .tracking = Tracking{wire::swap16(h.tracking_be)},
        .ts_ns = TimestampNs{wire::load_be48(h.ts_be)},
    };
}

inline void store_header(wire::Header* out, char type, const Header& hdr) noexcept {
    out->type = type;
    out->locate_be = wire::swap16(hdr.locate.value);
    out->tracking_be = wire::swap16(hdr.tracking.value);
    wire::store_be48(out->ts_be, hdr.ts_ns.value);
}

template <typename MsgT>
[[nodiscard]] inline std::expected<MsgT, DecodeError> check_and_wrap(MsgT msg,
                                                                     char wire_side) noexcept {
    if (wire_side != 'B' && wire_side != 'S') {
        return std::unexpected(DecodeError::invalid_side);
    }
    return msg;
}

} // namespace detail

// Scalar reference codecs; the SIMD/generated twins in M1 must stay bit-identical here.

[[nodiscard]] inline std::expected<AddOrderMsg, DecodeError>
decode_add_order(const unsigned char* buf, std::size_t len) noexcept {
    if (len < wire::kAddOrderSize)
        return std::unexpected(DecodeError::truncated);
    wire::AddOrderBody body;
    __builtin_memcpy(&body, buf, sizeof(body));
    auto side_check = detail::check_and_wrap(
        AddOrderMsg{
            .header = detail::decode_header(body.header),
            .order_ref = OrderId{wire::swap64(body.order_ref_be)},
            .side = side_from_wire(body.buy_sell),
            .shares = Qty{static_cast<std::int64_t>(wire::swap32(body.shares_be))},
            .stock = symbol_from(std::string_view(body.stock, 8)),
            .price = Price{static_cast<std::int64_t>(wire::swap32(body.price_be))},
        },
        body.buy_sell);
    return side_check;
}

inline void encode_add_order(const AddOrderMsg& m, unsigned char* out) noexcept {
    wire::AddOrderBody body{};
    detail::store_header(&body.header, 'A', m.header);
    body.order_ref_be = wire::swap64(m.order_ref.value);
    body.buy_sell = to_wire(m.side);
    body.shares_be = wire::swap32(static_cast<std::uint32_t>(m.shares.units));
    __builtin_memcpy(body.stock, m.stock.data(), 8);
    body.price_be = wire::swap32(static_cast<std::uint32_t>(m.price.ticks));
    __builtin_memcpy(out, &body, sizeof(body));
}

[[nodiscard]] inline std::expected<AddOrderAttributionMsg, DecodeError>
decode_add_order_attribution(const unsigned char* buf, std::size_t len) noexcept {
    if (len < wire::kAddOrderAttributionSize)
        return std::unexpected(DecodeError::truncated);
    wire::AddOrderAttributionBody body;
    __builtin_memcpy(&body, buf, sizeof(body));
    auto side_check = detail::check_and_wrap(
        AddOrderAttributionMsg{
            .header = detail::decode_header(body.base.header),
            .order_ref = OrderId{wire::swap64(body.base.order_ref_be)},
            .side = side_from_wire(body.base.buy_sell),
            .shares = Qty{static_cast<std::int64_t>(wire::swap32(body.base.shares_be))},
            .stock = symbol_from(std::string_view(body.base.stock, 8)),
            .price = Price{static_cast<std::int64_t>(wire::swap32(body.base.price_be))},
            .attribution = {body.attribution[0], body.attribution[1], body.attribution[2],
                            body.attribution[3]},
        },
        body.base.buy_sell);
    return side_check;
}

inline void encode_add_order_attribution(const AddOrderAttributionMsg& m,
                                         unsigned char* out) noexcept {
    wire::AddOrderAttributionBody body{};
    detail::store_header(&body.base.header, 'F', m.header);
    body.base.order_ref_be = wire::swap64(m.order_ref.value);
    body.base.buy_sell = to_wire(m.side);
    body.base.shares_be = wire::swap32(static_cast<std::uint32_t>(m.shares.units));
    __builtin_memcpy(body.base.stock, m.stock.data(), 8);
    body.base.price_be = wire::swap32(static_cast<std::uint32_t>(m.price.ticks));
    __builtin_memcpy(body.attribution, m.attribution.data(), 4);
    __builtin_memcpy(out, &body, sizeof(body));
}

[[nodiscard]] inline std::expected<OrderExecutedMsg, DecodeError>
decode_order_executed(const unsigned char* buf, std::size_t len) noexcept {
    if (len < wire::kOrderExecutedSize)
        return std::unexpected(DecodeError::truncated);
    wire::OrderExecutedBody body;
    __builtin_memcpy(&body, buf, sizeof(body));
    return OrderExecutedMsg{
        .header = detail::decode_header(body.header),
        .order_ref = OrderId{wire::swap64(body.order_ref_be)},
        .executed_shares = Qty{static_cast<std::int64_t>(wire::swap32(body.executed_shares_be))},
        .match_number = MatchNumber{wire::swap64(body.match_number_be)},
    };
}

inline void encode_order_executed(const OrderExecutedMsg& m, unsigned char* out) noexcept {
    wire::OrderExecutedBody body{};
    detail::store_header(&body.header, 'E', m.header);
    body.order_ref_be = wire::swap64(m.order_ref.value);
    body.executed_shares_be = wire::swap32(static_cast<std::uint32_t>(m.executed_shares.units));
    body.match_number_be = wire::swap64(m.match_number.value);
    __builtin_memcpy(out, &body, sizeof(body));
}

[[nodiscard]] inline std::expected<OrderExecutedWithPriceMsg, DecodeError>
decode_order_executed_with_price(const unsigned char* buf, std::size_t len) noexcept {
    if (len < wire::kOrderExecutedWithPriceSize)
        return std::unexpected(DecodeError::truncated);
    wire::OrderExecutedWithPriceBody body;
    __builtin_memcpy(&body, buf, sizeof(body));
    return OrderExecutedWithPriceMsg{
        .header = detail::decode_header(body.header),
        .order_ref = OrderId{wire::swap64(body.order_ref_be)},
        .executed_shares = Qty{static_cast<std::int64_t>(wire::swap32(body.executed_shares_be))},
        .match_number = MatchNumber{wire::swap64(body.match_number_be)},
        .printable = body.printable == 'Y',
        .execution_price = Price{static_cast<std::int64_t>(wire::swap32(body.execution_price_be))},
    };
}

inline void encode_order_executed_with_price(const OrderExecutedWithPriceMsg& m,
                                             unsigned char* out) noexcept {
    wire::OrderExecutedWithPriceBody body{};
    detail::store_header(&body.header, 'C', m.header);
    body.order_ref_be = wire::swap64(m.order_ref.value);
    body.executed_shares_be = wire::swap32(static_cast<std::uint32_t>(m.executed_shares.units));
    body.match_number_be = wire::swap64(m.match_number.value);
    body.printable = m.printable ? 'Y' : 'N';
    body.execution_price_be = wire::swap32(static_cast<std::uint32_t>(m.execution_price.ticks));
    __builtin_memcpy(out, &body, sizeof(body));
}

[[nodiscard]] inline std::expected<OrderCancelMsg, DecodeError>
decode_order_cancel(const unsigned char* buf, std::size_t len) noexcept {
    if (len < wire::kOrderCancelSize)
        return std::unexpected(DecodeError::truncated);
    wire::OrderCancelBody body;
    __builtin_memcpy(&body, buf, sizeof(body));
    return OrderCancelMsg{
        .header = detail::decode_header(body.header),
        .order_ref = OrderId{wire::swap64(body.order_ref_be)},
        .cancelled_shares = Qty{static_cast<std::int64_t>(wire::swap32(body.cancelled_shares_be))},
    };
}

inline void encode_order_cancel(const OrderCancelMsg& m, unsigned char* out) noexcept {
    wire::OrderCancelBody body{};
    detail::store_header(&body.header, 'X', m.header);
    body.order_ref_be = wire::swap64(m.order_ref.value);
    body.cancelled_shares_be = wire::swap32(static_cast<std::uint32_t>(m.cancelled_shares.units));
    __builtin_memcpy(out, &body, sizeof(body));
}

[[nodiscard]] inline std::expected<OrderDeleteMsg, DecodeError>
decode_order_delete(const unsigned char* buf, std::size_t len) noexcept {
    if (len < wire::kOrderDeleteSize)
        return std::unexpected(DecodeError::truncated);
    wire::OrderDeleteBody body;
    __builtin_memcpy(&body, buf, sizeof(body));
    return OrderDeleteMsg{
        .header = detail::decode_header(body.header),
        .order_ref = OrderId{wire::swap64(body.order_ref_be)},
    };
}

inline void encode_order_delete(const OrderDeleteMsg& m, unsigned char* out) noexcept {
    wire::OrderDeleteBody body{};
    detail::store_header(&body.header, 'D', m.header);
    body.order_ref_be = wire::swap64(m.order_ref.value);
    __builtin_memcpy(out, &body, sizeof(body));
}

[[nodiscard]] inline std::expected<OrderReplaceMsg, DecodeError>
decode_order_replace(const unsigned char* buf, std::size_t len) noexcept {
    if (len < wire::kOrderReplaceSize)
        return std::unexpected(DecodeError::truncated);
    wire::OrderReplaceBody body;
    __builtin_memcpy(&body, buf, sizeof(body));
    return OrderReplaceMsg{
        .header = detail::decode_header(body.header),
        .original_order_ref = OrderId{wire::swap64(body.original_order_ref_be)},
        .new_order_ref = OrderId{wire::swap64(body.new_order_ref_be)},
        .shares = Qty{static_cast<std::int64_t>(wire::swap32(body.shares_be))},
        .price = Price{static_cast<std::int64_t>(wire::swap32(body.price_be))},
    };
}

inline void encode_order_replace(const OrderReplaceMsg& m, unsigned char* out) noexcept {
    wire::OrderReplaceBody body{};
    detail::store_header(&body.header, 'U', m.header);
    body.original_order_ref_be = wire::swap64(m.original_order_ref.value);
    body.new_order_ref_be = wire::swap64(m.new_order_ref.value);
    body.shares_be = wire::swap32(static_cast<std::uint32_t>(m.shares.units));
    body.price_be = wire::swap32(static_cast<std::uint32_t>(m.price.ticks));
    __builtin_memcpy(out, &body, sizeof(body));
}

[[nodiscard]] inline std::expected<Message, DecodeError> decode_message(const unsigned char* buf,
                                                                        std::size_t len) noexcept {
    if (len < wire::kHeaderSize)
        return std::unexpected(DecodeError::truncated);
    Message out{};
    out.type = static_cast<char>(buf[0]);
    switch (out.type) {
    case 'A': {
        auto r = decode_add_order(buf, len);
        if (!r)
            return std::unexpected(r.error());
        out.add_order = *r;
        break;
    }
    case 'F': {
        auto r = decode_add_order_attribution(buf, len);
        if (!r)
            return std::unexpected(r.error());
        out.add_order_attribution = *r;
        break;
    }
    case 'E': {
        auto r = decode_order_executed(buf, len);
        if (!r)
            return std::unexpected(r.error());
        out.order_executed = *r;
        break;
    }
    case 'C': {
        auto r = decode_order_executed_with_price(buf, len);
        if (!r)
            return std::unexpected(r.error());
        out.order_executed_with_price = *r;
        break;
    }
    case 'X': {
        auto r = decode_order_cancel(buf, len);
        if (!r)
            return std::unexpected(r.error());
        out.order_cancel = *r;
        break;
    }
    case 'D': {
        auto r = decode_order_delete(buf, len);
        if (!r)
            return std::unexpected(r.error());
        out.order_delete = *r;
        break;
    }
    case 'U': {
        auto r = decode_order_replace(buf, len);
        if (!r)
            return std::unexpected(r.error());
        out.order_replace = *r;
        break;
    }
    default:
        return std::unexpected(DecodeError::unknown_type);
    }
    return out;
}

} // namespace mog
