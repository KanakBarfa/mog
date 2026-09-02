// NASDAQ OUCH 5.0 binary protocol definitions, layout audits, zero-copy codecs,
// and deterministic execution gateway.
#pragma once

#include <mog/Contracts.hpp>
#include <mog/Reflect.hpp>
#include <mog/Simulate.hpp>
#include <mog/Types.hpp>
#include <mog/Wire.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mog::ouch {

#pragma pack(push, 1)

struct EnterOrderWire {
    char type;
    char order_token[14];
    char buy_sell;
    std::uint32_t shares;
    char stock[8];
    std::uint32_t price;
    std::uint32_t time_in_force;
    char firm[4];
    char display;
    char capacity;
    char intermarket_sweep_eligibility;
    std::uint32_t minimum_quantity;
    char cross_type;
    char customer_type;
};
static_assert(sizeof(EnterOrderWire) == 49);
static_assert(offsetof(EnterOrderWire, order_token) == 1);
static_assert(offsetof(EnterOrderWire, buy_sell) == 15);
static_assert(offsetof(EnterOrderWire, shares) == 16);
static_assert(offsetof(EnterOrderWire, stock) == 20);
static_assert(offsetof(EnterOrderWire, price) == 28);
static_assert(offsetof(EnterOrderWire, time_in_force) == 32);
static_assert(offsetof(EnterOrderWire, firm) == 36);
static_assert(offsetof(EnterOrderWire, display) == 40);
static_assert(offsetof(EnterOrderWire, capacity) == 41);
static_assert(offsetof(EnterOrderWire, intermarket_sweep_eligibility) == 42);
static_assert(offsetof(EnterOrderWire, minimum_quantity) == 43);
static_assert(offsetof(EnterOrderWire, cross_type) == 47);
static_assert(offsetof(EnterOrderWire, customer_type) == 48);

struct ReplaceOrderWire {
    char type;
    char existing_order_token[14];
    char replacement_order_token[14];
    std::uint32_t shares;
    std::uint32_t price;
    std::uint32_t time_in_force;
    char display;
    char intermarket_sweep_eligibility;
    std::uint32_t minimum_quantity;
};
static_assert(sizeof(ReplaceOrderWire) == 47);
static_assert(offsetof(ReplaceOrderWire, existing_order_token) == 1);
static_assert(offsetof(ReplaceOrderWire, replacement_order_token) == 15);
static_assert(offsetof(ReplaceOrderWire, shares) == 29);
static_assert(offsetof(ReplaceOrderWire, price) == 33);
static_assert(offsetof(ReplaceOrderWire, time_in_force) == 37);
static_assert(offsetof(ReplaceOrderWire, display) == 41);
static_assert(offsetof(ReplaceOrderWire, intermarket_sweep_eligibility) == 42);
static_assert(offsetof(ReplaceOrderWire, minimum_quantity) == 43);

struct CancelOrderWire {
    char type;
    char order_token[14];
    std::uint32_t shares;
};
static_assert(sizeof(CancelOrderWire) == 19);
static_assert(offsetof(CancelOrderWire, order_token) == 1);
static_assert(offsetof(CancelOrderWire, shares) == 15);

struct ModifyOrderWire {
    char type;
    char order_token[14];
    char buy_sell;
    std::uint32_t shares;
};
static_assert(sizeof(ModifyOrderWire) == 20);

struct SystemEventWire {
    char type;
    std::uint64_t timestamp_ns;
    char event_code;
};
static_assert(sizeof(SystemEventWire) == 10);

struct OrderAcceptedWire {
    char type;
    std::uint64_t timestamp_ns;
    char order_token[14];
    char buy_sell;
    std::uint32_t shares;
    char stock[8];
    std::uint32_t price;
    std::uint32_t time_in_force;
    char firm[4];
    char display;
    std::uint64_t order_reference_number;
    char capacity;
    char intermarket_sweep_eligibility;
    std::uint32_t minimum_quantity;
    char cross_type;
    char order_state;
    char bbo_weight_indicator;
};
static_assert(sizeof(OrderAcceptedWire) == 66);
static_assert(offsetof(OrderAcceptedWire, timestamp_ns) == 1);
static_assert(offsetof(OrderAcceptedWire, order_token) == 9);
static_assert(offsetof(OrderAcceptedWire, buy_sell) == 23);
static_assert(offsetof(OrderAcceptedWire, shares) == 24);
static_assert(offsetof(OrderAcceptedWire, stock) == 28);
static_assert(offsetof(OrderAcceptedWire, price) == 36);
static_assert(offsetof(OrderAcceptedWire, time_in_force) == 40);
static_assert(offsetof(OrderAcceptedWire, firm) == 44);
static_assert(offsetof(OrderAcceptedWire, display) == 48);
static_assert(offsetof(OrderAcceptedWire, order_reference_number) == 49);
static_assert(offsetof(OrderAcceptedWire, capacity) == 57);
static_assert(offsetof(OrderAcceptedWire, intermarket_sweep_eligibility) == 58);
static_assert(offsetof(OrderAcceptedWire, minimum_quantity) == 59);
static_assert(offsetof(OrderAcceptedWire, cross_type) == 63);
static_assert(offsetof(OrderAcceptedWire, order_state) == 64);
static_assert(offsetof(OrderAcceptedWire, bbo_weight_indicator) == 65);

struct OrderExecutedWire {
    char type;
    std::uint64_t timestamp_ns;
    char order_token[14];
    std::uint32_t executed_shares;
    std::uint32_t execution_price;
    char liquidity_flag;
    std::uint64_t match_number;
};
static_assert(sizeof(OrderExecutedWire) == 40);
static_assert(offsetof(OrderExecutedWire, timestamp_ns) == 1);
static_assert(offsetof(OrderExecutedWire, order_token) == 9);
static_assert(offsetof(OrderExecutedWire, executed_shares) == 23);
static_assert(offsetof(OrderExecutedWire, execution_price) == 27);
static_assert(offsetof(OrderExecutedWire, liquidity_flag) == 31);
static_assert(offsetof(OrderExecutedWire, match_number) == 32);

struct OrderCancelledWire {
    char type;
    std::uint64_t timestamp_ns;
    char order_token[14];
    std::uint32_t decrement_shares;
    char reason;
};
static_assert(sizeof(OrderCancelledWire) == 28);
static_assert(offsetof(OrderCancelledWire, timestamp_ns) == 1);
static_assert(offsetof(OrderCancelledWire, order_token) == 9);
static_assert(offsetof(OrderCancelledWire, decrement_shares) == 23);
static_assert(offsetof(OrderCancelledWire, reason) == 27);

struct OrderReplacedWire {
    char type;
    std::uint64_t timestamp_ns;
    char replacement_order_token[14];
    char buy_sell;
    std::uint32_t shares;
    char stock[8];
    std::uint32_t price;
    std::uint32_t time_in_force;
    char firm[4];
    char display;
    std::uint64_t order_reference_number;
    char capacity;
    char intermarket_sweep_eligibility;
    std::uint32_t minimum_quantity;
    char cross_type;
    char order_state;
    char previous_order_token[14];
    char bbo_weight_indicator;
};
static_assert(sizeof(OrderReplacedWire) == 80);

struct OrderRejectedWire {
    char type;
    std::uint64_t timestamp_ns;
    char order_token[14];
    char reason;
};
static_assert(sizeof(OrderRejectedWire) == 24);

struct AIQCancelledWire {
    char type;
    std::uint64_t timestamp_ns;
    char order_token[14];
    std::uint32_t decrement_shares;
    char reason;
    std::uint32_t quantity_prevented_from_trading;
    std::uint32_t execution_price;
    char liquidity_flag;
};
static_assert(sizeof(AIQCancelledWire) == 37);
static_assert(offsetof(AIQCancelledWire, timestamp_ns) == 1);
static_assert(offsetof(AIQCancelledWire, order_token) == 9);
static_assert(offsetof(AIQCancelledWire, decrement_shares) == 23);
static_assert(offsetof(AIQCancelledWire, reason) == 27);
static_assert(offsetof(AIQCancelledWire, quantity_prevented_from_trading) == 28);
static_assert(offsetof(AIQCancelledWire, execution_price) == 32);
static_assert(offsetof(AIQCancelledWire, liquidity_flag) == 36);

#pragma pack(pop)

using Token = std::array<char, 14>;

inline std::string_view token_to_string_view(const Token& tok) noexcept {
    const auto* p = tok.data();
    std::size_t len = 0;
    while (len < tok.size() && p[len] != ' ' && p[len] != '\0')
        ++len;
    return std::string_view(p, len);
}

inline Token make_token(std::string_view s) noexcept {
    Token tok;
    tok.fill(' ');
    const std::size_t len = std::min(s.size(), tok.size());
    std::memcpy(tok.data(), s.data(), len);
    return tok;
}

inline Token token_from_id(std::uint64_t id) noexcept {
    char buf[16];
    const int n = std::snprintf(buf, sizeof(buf), "%014llu", static_cast<unsigned long long>(id));
    Token tok;
    tok.fill(' ');
    if (n > 0)
        std::memcpy(tok.data(), buf, std::min<std::size_t>(static_cast<std::size_t>(n), 14));
    return tok;
}

inline std::uint64_t id_from_token(const Token& tok) noexcept {
    std::uint64_t id = 0;
    for (char c : tok) {
        if (c >= '0' && c <= '9')
            id = id * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return id;
}

struct EnterOrderMsg {
    Token token;
    Side side;
    std::uint32_t shares;
    Symbol stock;
    std::int64_t price_ticks;
    std::uint32_t tif;
    char display;
};

struct ReplaceOrderMsg {
    Token existing_token;
    Token replacement_token;
    std::uint32_t shares;
    std::int64_t price_ticks;
    std::uint32_t tif;
    char display;
};

struct CancelOrderMsg {
    Token token;
    std::uint32_t shares;
};

// Encode functions writing big-endian wire bytes
inline void encode_enter_order(const EnterOrderMsg& msg, EnterOrderWire& out) noexcept {
    out.type = 'O';
    std::memcpy(out.order_token, msg.token.data(), 14);
    out.buy_sell = to_wire(msg.side);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.shares), msg.shares);
    std::memcpy(out.stock, msg.stock.data(), 8);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.price),
                     static_cast<std::uint32_t>(msg.price_ticks));
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.time_in_force), msg.tif);
    std::memset(out.firm, ' ', 4);
    out.display = msg.display;
    out.capacity = 'P';
    out.intermarket_sweep_eligibility = 'N';
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.minimum_quantity), 0);
    out.cross_type = 'N';
    out.customer_type = 'N';
}

inline void encode_cancel_order(const CancelOrderMsg& msg, CancelOrderWire& out) noexcept {
    out.type = 'X';
    std::memcpy(out.order_token, msg.token.data(), 14);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.shares), msg.shares);
}

inline void encode_order_accepted(std::uint64_t ts_ns, const Token& token, Side side,
                                  std::uint32_t shares, const Symbol& stock,
                                  std::int64_t price_ticks, std::uint64_t ref,
                                  OrderAcceptedWire& out) noexcept {
    out.type = 'A';
    wire::store_be64(reinterpret_cast<unsigned char*>(&out.timestamp_ns), ts_ns);
    std::memcpy(out.order_token, token.data(), 14);
    out.buy_sell = to_wire(side);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.shares), shares);
    std::memcpy(out.stock, stock.data(), 8);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.price),
                     static_cast<std::uint32_t>(price_ticks));
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.time_in_force), 0);
    std::memset(out.firm, ' ', 4);
    out.display = 'Y';
    wire::store_be64(reinterpret_cast<unsigned char*>(&out.order_reference_number), ref);
    out.capacity = 'P';
    out.intermarket_sweep_eligibility = 'N';
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.minimum_quantity), 0);
    out.cross_type = 'N';
    out.order_state = 'L';
    out.bbo_weight_indicator = ' ';
}

inline void encode_order_executed(std::uint64_t ts_ns, const Token& token, std::uint32_t shares,
                                  std::int64_t price_ticks, char liquidity, std::uint64_t match,
                                  OrderExecutedWire& out) noexcept {
    out.type = 'E';
    wire::store_be64(reinterpret_cast<unsigned char*>(&out.timestamp_ns), ts_ns);
    std::memcpy(out.order_token, token.data(), 14);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.executed_shares), shares);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.execution_price),
                     static_cast<std::uint32_t>(price_ticks));
    out.liquidity_flag = liquidity;
    wire::store_be64(reinterpret_cast<unsigned char*>(&out.match_number), match);
}

inline void encode_order_cancelled(std::uint64_t ts_ns, const Token& token, std::uint32_t shares,
                                   char reason, OrderCancelledWire& out) noexcept {
    out.type = 'C';
    wire::store_be64(reinterpret_cast<unsigned char*>(&out.timestamp_ns), ts_ns);
    std::memcpy(out.order_token, token.data(), 14);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.decrement_shares), shares);
    out.reason = reason;
}

inline void encode_order_rejected(std::uint64_t ts_ns, const Token& token, char reason,
                                  OrderRejectedWire& out) noexcept {
    out.type = 'J';
    wire::store_be64(reinterpret_cast<unsigned char*>(&out.timestamp_ns), ts_ns);
    std::memcpy(out.order_token, token.data(), 14);
    out.reason = reason;
}

inline void encode_replace_order(const ReplaceOrderMsg& in, ReplaceOrderWire& out) noexcept {
    out.type = 'U';
    std::memcpy(out.existing_order_token, in.existing_token.data(), 14);
    std::memcpy(out.replacement_order_token, in.replacement_token.data(), 14);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.shares), in.shares);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.price),
                     static_cast<std::uint32_t>(in.price_ticks));
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.time_in_force), in.tif);
    out.display = in.display;
    out.intermarket_sweep_eligibility = 'N';
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.minimum_quantity), 0);
}

inline void encode_order_replaced(std::uint64_t ts_ns, const Token& replacement_token, Side side,
                                  std::uint32_t shares, const Symbol& stock,
                                  std::int64_t price_ticks, std::uint64_t ref,
                                  const Token& previous_token, OrderReplacedWire& out) noexcept {
    out.type = 'U';
    wire::store_be64(reinterpret_cast<unsigned char*>(&out.timestamp_ns), ts_ns);
    std::memcpy(out.replacement_order_token, replacement_token.data(), 14);
    out.buy_sell = to_wire(side);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.shares), shares);
    std::memcpy(out.stock, stock.data(), 8);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.price),
                     static_cast<std::uint32_t>(price_ticks));
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.time_in_force), 99999);
    std::memset(out.firm, ' ', 4);
    out.display = 'Y';
    wire::store_be64(reinterpret_cast<unsigned char*>(&out.order_reference_number), ref);
    out.capacity = 'O';
    out.intermarket_sweep_eligibility = 'N';
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.minimum_quantity), 0);
    out.cross_type = 'N';
    out.order_state = 'L';
    std::memcpy(out.previous_order_token, previous_token.data(), 14);
    out.bbo_weight_indicator = ' ';
}

inline void encode_aiq_cancelled(std::uint64_t ts_ns, const Token& token,
                                 std::uint32_t decrement_shares, char reason,
                                 std::uint32_t qty_prevented, std::int64_t exec_price,
                                 char liq_flag, AIQCancelledWire& out) noexcept {
    out.type = 'D';
    wire::store_be64(reinterpret_cast<unsigned char*>(&out.timestamp_ns), ts_ns);
    std::memcpy(out.order_token, token.data(), 14);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.decrement_shares), decrement_shares);
    out.reason = reason;
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.quantity_prevented_from_trading),
                     qty_prevented);
    wire::store_be32(reinterpret_cast<unsigned char*>(&out.execution_price),
                     static_cast<std::uint32_t>(exec_price));
    out.liquidity_flag = liq_flag;
}

// Inbound OUCH frame length lookup table
[[nodiscard]] constexpr std::size_t inbound_frame_length(char type) noexcept {
    switch (type) {
    case 'O':
        return sizeof(EnterOrderWire);
    case 'U':
        return sizeof(ReplaceOrderWire);
    case 'X':
        return sizeof(CancelOrderWire);
    case 'M':
        return sizeof(ModifyOrderWire);
    default:
        return 0;
    }
}

// Outbound OUCH frame length lookup table
[[nodiscard]] constexpr std::size_t outbound_frame_length(char type) noexcept {
    switch (type) {
    case 'S':
        return sizeof(SystemEventWire);
    case 'A':
        return sizeof(OrderAcceptedWire);
    case 'E':
        return sizeof(OrderExecutedWire);
    case 'C':
        return sizeof(OrderCancelledWire);
    case 'U':
        return sizeof(OrderReplacedWire);
    case 'D':
        return sizeof(AIQCancelledWire);
    case 'J':
        return sizeof(OrderRejectedWire);
    default:
        return 0;
    }
}

// Flat cache-conscious open-addressing table for sub-nanosecond order tracking
class FlatOrderTable {
public:
    struct OrderMeta {
        Token token;
        std::uint32_t shares = 0;
        std::int64_t price_ticks = 0;
        Side side = Side::buy;
        Symbol stock = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    };

    struct Slot {
        std::uint64_t ref = 0;
        OrderMeta meta{};
    };

    explicit FlatOrderTable(std::size_t cap = 2048) {
        std::size_t pow2 = 64;
        while (pow2 < cap)
            pow2 <<= 1;
        mask_ = pow2 - 1;
        slots_.resize(pow2);
    }

    [[nodiscard]] const OrderMeta* find(std::uint64_t ref) const noexcept {
        if (ref == 0)
            return nullptr;
        std::size_t idx = hash(ref) & mask_;
        for (std::size_t i = 0; i <= mask_; ++i) {
            const auto& s = slots_[(idx + i) & mask_];
            if (s.ref == ref)
                return &s.meta;
            if (s.ref == 0)
                return nullptr;
        }
        return nullptr;
    }

    [[nodiscard]] OrderMeta* find(std::uint64_t ref) noexcept {
        if (ref == 0)
            return nullptr;
        std::size_t idx = hash(ref) & mask_;
        for (std::size_t i = 0; i <= mask_; ++i) {
            auto& s = slots_[(idx + i) & mask_];
            if (s.ref == ref)
                return &s.meta;
            if (s.ref == 0)
                return nullptr;
        }
        return nullptr;
    }

    void insert_or_assign(std::uint64_t ref, const OrderMeta& meta) {
        if (ref == 0)
            return;
        if (size_ * 2 >= slots_.size())
            grow();
        std::size_t idx = hash(ref) & mask_;
        for (std::size_t i = 0; i <= mask_; ++i) {
            auto& s = slots_[(idx + i) & mask_];
            if (s.ref == ref) {
                s.meta = meta;
                return;
            }
            if (s.ref == 0) {
                s.ref = ref;
                s.meta = meta;
                ++size_;
                return;
            }
        }
    }

    void erase(std::uint64_t ref) noexcept {
        if (ref == 0)
            return;
        std::size_t idx = hash(ref) & mask_;
        for (std::size_t i = 0; i <= mask_; ++i) {
            std::size_t pos = (idx + i) & mask_;
            if (slots_[pos].ref == ref) {
                slots_[pos].ref = 0;
                --size_;
                std::size_t j = (pos + 1) & mask_;
                while (slots_[j].ref != 0) {
                    Slot move_slot = slots_[j];
                    slots_[j].ref = 0;
                    --size_;
                    insert_or_assign(move_slot.ref, move_slot.meta);
                    j = (j + 1) & mask_;
                }
                return;
            }
            if (slots_[pos].ref == 0)
                return;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    static std::uint64_t hash(std::uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.resize(old.size() * 2);
        mask_ = slots_.size() - 1;
        size_ = 0;
        for (const auto& s : old) {
            if (s.ref != 0)
                insert_or_assign(s.ref, s.meta);
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

// Deterministic OUCH Gateway translating between wire frames and ExecutionSimulator
class OuchGateway {
public:
    using OrderMeta = FlatOrderTable::OrderMeta;

    struct ReplacedEvent {
        Token replacement_token;
        Token previous_token;
        Side side = Side::buy;
        std::uint32_t shares = 0;
        Symbol stock = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
        std::int64_t price_ticks = 0;
        std::uint64_t ref = 0;
        std::uint64_t ts_ns = 0;
    };

    explicit OuchGateway(ExecutionSimulator& sim, Symbol default_symbol = {'S', 'P', 'Y', ' '})
        : sim_(sim), default_symbol_(default_symbol) {}

    // Submits an inbound EnterOrder wire message directly to the simulator
    [[nodiscard]] std::uint64_t submit_enter(const EnterOrderWire& wire, std::uint64_t arrival_ts) {
        Token tok;
        std::memcpy(tok.data(), wire.order_token, 14);
        const std::uint64_t ref = id_from_token(tok);
        const Side side = side_from_wire(wire.buy_sell);
        const std::uint32_t shares =
            wire::load_be32(reinterpret_cast<const unsigned char*>(&wire.shares));
        const std::int64_t price =
            wire::load_be32(reinterpret_cast<const unsigned char*>(&wire.price));
        const std::uint32_t tif =
            wire::load_be32(reinterpret_cast<const unsigned char*>(&wire.time_in_force));

        Symbol sym;
        std::memcpy(sym.data(), wire.stock, 8);
        orders_.insert_or_assign(ref, OrderMeta{tok, shares, price, side, sym});

        const SimOrderType type = tif == 99999 ? SimOrderType::ioc : SimOrderType::day_limit;
        const SimInbound in{OrderId{ref}, side, Qty{shares}, Price{price}, type};
        fold_hash(wire.type, ref, shares, price);
        return sim_.submit(in, arrival_ts);
    }

    // Submits an inbound ReplaceOrder wire message directly to the simulator
    [[nodiscard]] bool submit_replace(const ReplaceOrderWire& wire, std::uint64_t arrival_ts = 0) {
        Token old_tok;
        std::memcpy(old_tok.data(), wire.existing_order_token, 14);
        Token new_tok;
        std::memcpy(new_tok.data(), wire.replacement_order_token, 14);
        const std::uint64_t old_ref = id_from_token(old_tok);
        const std::uint64_t new_ref = id_from_token(new_tok);
        const std::uint32_t shares =
            wire::load_be32(reinterpret_cast<const unsigned char*>(&wire.shares));
        const std::int64_t price =
            wire::load_be32(reinterpret_cast<const unsigned char*>(&wire.price));

        const auto* meta = orders_.find(old_ref);
        const Side side = (meta != nullptr) ? meta->side : Side::buy;
        const Symbol stock = (meta != nullptr) ? meta->stock : default_symbol_;

        const BookTick tick =
            sim_.replace_strategy(OrderId{old_ref}, OrderId{new_ref}, Qty{shares}, Price{price});
        if (!ok(tick))
            return false;

        orders_.insert_or_assign(new_ref, OrderMeta{new_tok, shares, price, side, stock});
        replaced_events_.push_back(
            ReplacedEvent{new_tok, old_tok, side, shares, stock, price, new_ref, arrival_ts});
        fold_hash(wire.type, new_ref, shares, price);
        return true;
    }

    // Submits an inbound CancelOrder wire message directly to the simulator
    [[nodiscard]] bool submit_cancel(const CancelOrderWire& wire) {
        Token tok;
        std::memcpy(tok.data(), wire.order_token, 14);
        const std::uint64_t ref = id_from_token(tok);
        fold_hash(wire.type, ref, 0, 0);
        return sim_.cancel_strategy(OrderId{ref});
    }

    // Streaming zero-allocation drain of outbound wire frames directly to a callback sink
    template <typename Sink>
    void drain_outbound_stream(Sink&& sink) {
        const auto& decisions = sim_.decisions();
        const auto& reports = sim_.reports();

        for (; last_decision_idx_ < decisions.size(); ++last_decision_idx_) {
            const auto& d = decisions[last_decision_idx_];
            Token tok = token_from_id(d.ref);
            const auto* meta = orders_.find(d.ref);
            const std::uint32_t orig_shares = (meta != nullptr) ? meta->shares : d.filled_qty;
            const std::int64_t orig_price = (meta != nullptr) ? meta->price_ticks : 0;
            const Symbol& stock = (meta != nullptr) ? meta->stock : default_symbol_;

            if (d.kind == SimDecision::Kind::rested || d.kind == SimDecision::Kind::filled ||
                d.kind == SimDecision::Kind::partial_then_rest) {
                OrderAcceptedWire wire{};
                encode_order_accepted(d.visible_ts, tok, side_from_wire(d.side), orig_shares, stock,
                                      orig_price, d.ref, wire);
                sink(&wire, sizeof(wire));
            } else if (d.kind == SimDecision::Kind::rejected_stp) {
                AIQCancelledWire wire{};
                encode_aiq_cancelled(d.visible_ts, tok, d.cancelled_qty, 'Q', d.cancelled_qty,
                                     orig_price, 'N', wire);
                sink(&wire, sizeof(wire));
            } else if (d.kind == SimDecision::Kind::rejected_would_cross ||
                       d.kind == SimDecision::Kind::rejected_unknown) {
                OrderRejectedWire wire{};
                encode_order_rejected(d.visible_ts, tok, 'T', wire);
                sink(&wire, sizeof(wire));
            } else if (d.kind == SimDecision::Kind::partial_then_cancelled) {
                OrderCancelledWire wire{};
                encode_order_cancelled(d.visible_ts, tok, d.cancelled_qty, 'U', wire);
                sink(&wire, sizeof(wire));
            }
        }

        for (; last_replaced_idx_ < replaced_events_.size(); ++last_replaced_idx_) {
            const auto& rep = replaced_events_[last_replaced_idx_];
            OrderReplacedWire wire{};
            encode_order_replaced(rep.ts_ns, rep.replacement_token, rep.side, rep.shares, rep.stock,
                                  rep.price_ticks, rep.ref, rep.previous_token, wire);
            sink(&wire, sizeof(wire));
        }

        for (; last_report_idx_ < reports.size(); ++last_report_idx_) {
            const auto& r = reports[last_report_idx_];
            Token tok = token_from_id(r.ref);
            OrderExecutedWire wire{};
            encode_order_executed(r.visible_ts, tok, r.qty, r.price_ticks, 'A', r.seq, wire);
            sink(&wire, sizeof(wire));
        }
    }

    // Drains simulator decisions and fill reports into individual vector packets
    void drain_outbound(std::vector<std::vector<unsigned char>>& out_packets) {
        drain_outbound_stream([&](const void* data, std::size_t len) {
            std::vector<unsigned char> p(len);
            std::memcpy(p.data(), data, len);
            out_packets.push_back(std::move(p));
        });
    }

    // Drains simulator decisions and fill reports into a contiguous byte buffer with packet offsets
    void drain_outbound_contiguous(std::vector<std::uint8_t>& out_bytes,
                                   std::vector<std::size_t>& offsets) {
        drain_outbound_stream([&](const void* data, std::size_t len) {
            offsets.push_back(out_bytes.size());
            const auto* byte_ptr = static_cast<const std::uint8_t*>(data);
            out_bytes.insert(out_bytes.end(), byte_ptr, byte_ptr + len);
        });
    }

    [[nodiscard]] std::uint64_t trace_hash() const noexcept { return hash_; }

private:
    void fold_hash(char type, std::uint64_t ref, std::uint64_t qty, std::int64_t px) noexcept {
        std::uint64_t h = hash_ ^ static_cast<std::uint64_t>(type);
        h = (h * 0xbf58476d1ce4e5b9ULL) ^ ref;
        h = (h * 0xbf58476d1ce4e5b9ULL) ^ qty;
        h = (h * 0xbf58476d1ce4e5b9ULL) ^ static_cast<std::uint64_t>(px);
        hash_ = h;
    }

    ExecutionSimulator& sim_;
    Symbol default_symbol_;
    FlatOrderTable orders_;
    std::vector<ReplacedEvent> replaced_events_;
    std::size_t last_decision_idx_ = 0;
    std::size_t last_replaced_idx_ = 0;
    std::size_t last_report_idx_ = 0;
    std::uint64_t hash_ = 0xcbf29ce484222325ULL;
};

} // namespace mog::ouch
