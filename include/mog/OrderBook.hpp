// L3 limit order book: arena-backed orders, per-side paged ladders, intrusive
// FIFO queues, incremental L2 aggregates, and contracts at the true invariant sites.
#pragma once

#include <mog/Arena.hpp>
#include <mog/Contracts.hpp>
#include <mog/Ladder.hpp>
#include <mog/SnapshotRing.hpp>
#include <mog/Types.hpp>
#include <mog/polyfill/InplaceVector.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <new>

namespace mog {

struct OrderHandleTag {};
using OrderHandle = GenHandle<OrderHandleTag>;

struct Order {
    std::uint64_t order_ref = 0;
    std::int64_t qty_remaining = 0;
    std::int64_t price_ticks = 0;
    std::uint32_t prev = kNullIndex;
    std::uint32_t next = kNullIndex;
    char side = 'B';
};

enum class BookError : std::uint8_t {
    unknown_order = 1,
    duplicate_ref = 2,
    over_execute = 3,
    bad_qty = 4,
    out_of_band = 5,
    arena_full = 6,
    empty_level = 7,
    page_exhausted = 8, // ladder pool too small for this instrument's spread
    zero_ref = 9,       // ref 0 is the unset sentinel and is never bookable
};

[[nodiscard]] constexpr std::string_view error_text(BookError e) noexcept {
    switch (e) {
    case BookError::unknown_order:
        return "unknown_order";
    case BookError::duplicate_ref:
        return "duplicate_ref";
    case BookError::over_execute:
        return "over_execute";
    case BookError::bad_qty:
        return "bad_qty";
    case BookError::out_of_band:
        return "out_of_band";
    case BookError::empty_level:
        return "empty_level";
    case BookError::arena_full:
        return "arena_full";
    case BookError::page_exhausted:
        return "page_exhausted";
    case BookError::zero_ref:
        return "zero_ref";
    }
    return "?";
}

enum class BookEventKind : std::uint8_t { added = 0, reduced = 1, removed = 2, replaced = 3 };

// Sentinel kind carried by failed ops; error holds the BookError code then.
inline constexpr std::uint8_t kTickError = 0xFF;

// 16-byte POD so successful calls return in register pairs without sret.
// Deltas move behind an optional out-pointer; hot replay passes nullptr.
struct BookTick {
    std::int64_t price_ticks = 0;
    std::uint32_t qty = 0;
    std::uint8_t kind = static_cast<std::uint8_t>(BookEventKind::added);
    std::uint8_t side = 'B';
    std::uint8_t error = 0;
    std::uint8_t reserved = 0;
};
static_assert(sizeof(BookTick) == 16);

[[nodiscard]] constexpr bool ok(const BookTick& t) noexcept {
    return t.kind != kTickError;
}

template <std::size_t kPageShift = 10>
class OrderBook {
public:
    struct Config {
        std::size_t arena_capacity;
        typename PriceLadder<kPageShift>::Config ladder;
    };

    explicit OrderBook(Config cfg)
        : arena_(cfg.arena_capacity), bid_ladder_(cfg.ladder), ask_ladder_(cfg.ladder),
          table_cap_(round_pow2(cfg.arena_capacity * 2)), table_shift_(64 - floor_log2(table_cap_)),
          entries_(static_cast<Entry*>(::operator new(table_cap_ * sizeof(Entry)))) {
        static constexpr std::int64_t kMaxTicks = INT32_MAX;
        MOG_PRE(cfg.ladder.hi_tick <= kMaxTicks);
        // Construction-time only; replay never touches this loop.
        static constexpr Entry kEmptyEntry{};
        std::fill(entries_, entries_ + table_cap_, kEmptyEntry);
    }

    ~OrderBook() { ::operator delete(entries_); }

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    [[nodiscard]] std::size_t live_orders() const noexcept { return arena_.live_count(); }

    static constexpr std::size_t floor_log2(std::size_t v) noexcept {
        std::size_t r = 0;
        while ((v >>= 1) != 0)
            ++r;
        return r;
    }
    [[nodiscard]] std::int64_t best_bid() const noexcept { return best_bid_; }
    [[nodiscard]] std::int64_t best_ask() const noexcept { return best_ask_; }

    // Total resting quantity at an exact price; 0 when the level is absent.
    // Simulation layers use this for queue-position arithmetic.
    [[nodiscard]] std::int64_t qty_at(Side s, Price price) const noexcept {
        const Level* lv = ladder_for(s).peek(price.ticks);
        return lv != nullptr ? lv->qty_total : 0;
    }

    // Remaining quantity of a live order; 0 for unknown refs. Read-only view
    // for sim-layer conservation checks.
    [[nodiscard]] std::int64_t remaining_of(OrderId ref) const noexcept {
        const OrderHandle h = id_find(ref.value);
        if (h.index == kNullIndex || !arena_.valid(h))
            return 0;
        return arena_.at(h).qty_remaining;
    }

    // Probe-free remainder when the caller already holds a fresh handle.
    [[nodiscard]] std::int64_t remaining_of_handle(OrderHandle h) const noexcept {
        MOG_PRE(arena_.valid(h));
        return arena_.at(h).qty_remaining;
    }

    // Raw handle lookup for simulation layers that key side data by slot.
    [[nodiscard]] OrderHandle find_handle(OrderId ref) const noexcept { return id_find(ref.value); }

    // Wire-side char of a live order ('B'/'S'), '?' when unknown.
    [[nodiscard]] char side_of(OrderId ref) const noexcept {
        const OrderHandle h = id_find(ref.value);
        if (h.index == kNullIndex || !arena_.valid(h))
            return '?';
        return arena_.at(h).side;
    }

    // Visits each live order at an exact level in FIFO order (head first),
    // passing (ref, remaining). Simulation layers derive exact queue
    // positions from this walk; there is no estimation anywhere downstream.
    template <class Fn>
    void for_each_in_level(Side s, Price price, Fn&& fn) const {
        const Level* lv = ladder_for(s).peek(price.ticks);
        if (lv == nullptr)
            return;
        std::uint32_t idx = lv->head;
        while (idx != kNullIndex) {
            const Order& o = arena_.at_index(idx);
            fn(OrderId{o.order_ref}, o.qty_remaining);
            idx = o.next;
        }
    }

    // Same walk plus the arena slot index; slot-keyed callers skip probing.
    template <class Fn>
    void for_each_in_level_idx(Side s, Price price, Fn&& fn) const {
        const Level* lv = ladder_for(s).peek(price.ticks);
        if (lv == nullptr)
            return;
        std::uint32_t idx = lv->head;
        while (idx != kNullIndex) {
            const Order& o = arena_.at_index(idx);
            fn(idx, OrderId{o.order_ref}, o.qty_remaining);
            idx = o.next;
        }
    }

    // FIFO head order ref at an exact price; 0 when the level is empty.
    [[nodiscard]] std::uint64_t front_ref(Side s, Price price) const noexcept {
        const Level* lv = ladder_for(s).peek(price.ticks);
        return (lv == nullptr || lv->head == kNullIndex) ? 0 : arena_.at_index(lv->head).order_ref;
    }
    [[nodiscard]] bool in_band(std::int64_t tick) const noexcept {
        return bid_ladder_.in_band(tick);
    }
    [[nodiscard]] std::int64_t next_price_below(Side s, std::int64_t tick) const noexcept {
        return ladder_for(s).first_below(tick);
    }
    [[nodiscard]] std::int64_t next_price_above(Side s, std::int64_t tick) const noexcept {
        return ladder_for(s).first_above(tick);
    }

    // A first-touch at a new price claims a ladder page; refuse loudly when
    // the pool is spent instead of letting a compiled-out contract turn
    // exhaustion into silent out-of-bounds writes.
    [[nodiscard]] BookTick add(OrderId ref, Side side, Qty qty, Price price,
                               inplace_vector<LevelDelta, 3>* deltas = nullptr) noexcept {
        if (ref.value == 0)
            return tick_error(BookError::zero_ref, 0);
        if (qty.units <= 0)
            return tick_error(BookError::bad_qty, ref.value);
        if (!in_band(price.ticks))
            return tick_error(BookError::out_of_band, ref.value);
        if (!ladder_for(side).claimable(price.ticks))
            return tick_error(BookError::page_exhausted, ref.value);
        const auto res = id_find_slot(ref.value);
        if (res.handle.index != kNullIndex)
            return tick_error(BookError::duplicate_ref, ref.value);
        const OrderHandle h = arena_.acquire(ref.value, qty.units, price.ticks, kNullIndex,
                                             kNullIndex, to_wire(side));
        if (h.is_null())
            return tick_error(BookError::arena_full, ref.value);
        id_insert_at(res.slot, ref.value, h);
        PriceLadder<kPageShift>& lad = ladder_for(side);
        Level* lv = lad.apply(price.ticks, qty.units);
        push_tail(*lv, h.index);
        if (side == Side::buy) {
            if (best_bid_ == kNoTick || price.ticks > best_bid_)
                best_bid_ = price.ticks;
        } else {
            if (best_ask_ == kNoTick || price.ticks < best_ask_)
                best_ask_ = price.ticks;
        }
        if (deltas != nullptr)
            static_cast<void>(
                deltas->emplace_back(LevelDelta{.price_ticks = price.ticks,
                                                .qty_delta = static_cast<std::int32_t>(qty.units),
                                                .side = static_cast<std::uint8_t>(to_wire(side))}));
        return BookTick{.price_ticks = price.ticks,
                        .qty = static_cast<std::uint32_t>(qty.units),
                        .kind = static_cast<std::uint8_t>(BookEventKind::added),
                        .side = static_cast<std::uint8_t>(to_wire(side)),
                        .error = 0,
                        .reserved = 0};
    }

    [[nodiscard]] BookTick execute(OrderId ref, Qty qty,
                                   inplace_vector<LevelDelta, 3>* deltas = nullptr) noexcept {
        if (qty.units <= 0)
            return tick_error(BookError::bad_qty, ref.value);
        const auto res = id_find_slot(ref.value);
        if (res.handle.index == kNullIndex)
            return tick_error(BookError::unknown_order, ref.value);
        Order& o = arena_.at(res.handle);
        if (qty.units > o.qty_remaining)
            return tick_error(BookError::over_execute, ref.value);
        return reduce(res.handle, o, qty.units, BookEventKind::reduced, deltas, res.slot);
    }

    // Partial cancel shares execution semantics; zero remainder removes the order.
    [[nodiscard]] BookTick cancel(OrderId ref, Qty qty,
                                  inplace_vector<LevelDelta, 3>* deltas = nullptr) noexcept {
        return execute(ref, qty, deltas);
    }

    // Exchange-style match against the FIFO head at an exact price: external
    // flow never names a victim ref, the queue does. Reports the hit order's
    // ref through *victim when non-null. Callers clamp qty to qty_at() to
    // keep the strict over-execute contract meaningful per event.
    [[nodiscard]] BookTick execute_front(Side s, Price price, Qty qty,
                                         inplace_vector<LevelDelta, 3>* deltas = nullptr,
                                         OrderId* victim = nullptr) noexcept {
        if (qty.units <= 0)
            return tick_error(BookError::bad_qty, 0);
        const Level* lv = ladder_for(s).peek(price.ticks);
        if (lv == nullptr || lv->head == kNullIndex)
            return tick_error(BookError::empty_level, 0);
        // Head index in hand: no id-table probe; full-removal erase resolves by ref.
        const std::uint32_t head_idx = lv->head;
        const Order& head_o = arena_.at_index(head_idx);
        const OrderHandle direct{head_idx, arena_.slot(head_idx).generation};
#if MOG_CONTRACTS_DEFAULT_MODE != 2
        const auto check = id_find_slot(head_o.order_ref);
        MOG_CONTRACT_ASSERT(check.handle == direct);
#endif
        if (victim != nullptr)
            *victim = OrderId{head_o.order_ref};
        return reduce(direct, arena_.at(direct), qty.units, BookEventKind::reduced, deltas);
    }

    [[nodiscard]] BookTick remove(OrderId ref,
                                  inplace_vector<LevelDelta, 3>* deltas = nullptr) noexcept {
        const auto res = id_find_slot(ref.value);
        if (res.handle.index == kNullIndex)
            return tick_error(BookError::unknown_order, ref.value);
        Order& o = arena_.at(res.handle);
        return reduce(res.handle, o, o.qty_remaining, BookEventKind::removed, deltas, res.slot);
    }

    // Atomic validation first: a rejected replace leaves both orders untouched.
    [[nodiscard]] BookTick replace(OrderId orig_ref, OrderId new_ref, Qty new_qty, Price new_price,
                                   inplace_vector<LevelDelta, 3>* deltas = nullptr) noexcept {
        const auto err = [&](BookError e) {
            return BookTick{.price_ticks = 0,
                            .qty = 0,
                            .kind = kTickError,
                            .side = 0,
                            .error = static_cast<std::uint8_t>(e),
                            .reserved = 0};
        };
        if (new_qty.units <= 0)
            return err(BookError::bad_qty);
        if (!in_band(new_price.ticks))
            return err(BookError::out_of_band);
        const OrderHandle oh = id_find(orig_ref.value);
        if (oh.index == kNullIndex)
            return err(BookError::unknown_order);
        const Side orig_side = arena_.at(oh).side == 'B' ? Side::buy : Side::sell;
        // The replacement may land on a fresh page; gate before mutating.
        if (!ladder_for(orig_side).claimable(new_price.ticks))
            return err(BookError::page_exhausted);
        if (orig_ref.value != new_ref.value && id_find(new_ref.value).index != kNullIndex)
            return err(BookError::duplicate_ref);

        inplace_vector<LevelDelta, 3> staged{};
        const BookTick removed = remove(orig_ref, &staged);
        MOG_CONTRACT_ASSERT(ok(removed)); // validated above
        if (deltas != nullptr)
            for (const LevelDelta& d : staged)
                static_cast<void>(deltas->push_back(d)); // removal precedes addition
        const BookTick added = add(new_ref, orig_side, new_qty, new_price, deltas);
        MOG_CONTRACT_ASSERT(ok(added)); // capacity reserved above; band checked above
        return BookTick{.price_ticks = added.price_ticks,
                        .qty = added.qty,
                        .kind = static_cast<std::uint8_t>(BookEventKind::replaced),
                        .side = added.side,
                        .error = 0,
                        .reserved = 0};
    }

    // C messages change the printed trade price only; the book effect equals E.
    [[nodiscard]] BookTick apply(const Message& m,
                                 inplace_vector<LevelDelta, 3>* deltas = nullptr) noexcept {
        switch (m.type) {
        case 'A':
            return add(m.add_order.order_ref, m.add_order.side, m.add_order.shares,
                       m.add_order.price, deltas);
        case 'F':
            return add(m.add_order_attribution.order_ref, m.add_order_attribution.side,
                       m.add_order_attribution.shares, m.add_order_attribution.price, deltas);
        case 'E':
            return execute(m.order_executed.order_ref, m.order_executed.executed_shares, deltas);
        case 'C':
            return execute(m.order_executed_with_price.order_ref,
                           m.order_executed_with_price.executed_shares, deltas);
        case 'X':
            return cancel(m.order_cancel.order_ref, m.order_cancel.cancelled_shares, deltas);
        case 'D':
            return remove(m.order_delete.order_ref, deltas);
        case 'U':
            return replace(m.order_replace.original_order_ref, m.order_replace.new_order_ref,
                           m.order_replace.shares, m.order_replace.price, deltas);
        default:
            MOG_PRE(false && "unreachable message type for the book");
            return tick_error(BookError::unknown_order, 0);
        }
    }

    template <class Fn>
    void for_each_l2(Fn&& fn) const noexcept {
        bid_ladder_.for_each_level(
            [&](std::int64_t tick, const Level& lv) { fn(Side::buy, tick, lv.qty_total); });
        ask_ladder_.for_each_level(
            [&](std::int64_t tick, const Level& lv) { fn(Side::sell, tick, lv.qty_total); });
    }

    // Full consistency walk: FIFO linkage, aggregate equality, best prices,
    // table membership, free-list integrity. O(live + band pages).
    bool audit() const noexcept {
        if (!check_fifos(Side::buy) || !check_fifos(Side::sell))
            return false;
        if (best_bid_ != brute_best(Side::buy) || best_ask_ != brute_best(Side::sell))
            return false;
        std::size_t occupied = 0;
        for (std::size_t i = 0; i < table_cap_; ++i) {
            if (entries_[i].ref == 0)
                continue;
            ++occupied;
            const OrderHandle h = entries_[i].handle;
            if (!arena_.valid(h) || arena_.at(h).order_ref != entries_[i].ref)
                return false;
        }
        return occupied == arena_.live_count();
    }

private:
    struct Entry {
        std::uint64_t ref = 0; // zero marks the empty slot; refs are never zero
        OrderHandle handle{};
    };
    using LadderT = PriceLadder<kPageShift>;

    [[nodiscard]] static BookTick tick_error(BookError e, std::uint64_t /*ref*/) noexcept {
        return BookTick{.price_ticks = 0,
                        .qty = 0,
                        .kind = kTickError,
                        .side = 0,
                        .error = static_cast<std::uint8_t>(e),
                        .reserved = 0};
    }

    [[nodiscard]] LadderT& ladder_for(Side s) noexcept {
        return s == Side::buy ? bid_ladder_ : ask_ladder_;
    }
    [[nodiscard]] const LadderT& ladder_for(Side s) const noexcept {
        return s == Side::buy ? bid_ladder_ : ask_ladder_;
    }

    static std::size_t round_pow2(std::size_t n) noexcept {
        MOG_PRE(n > 0);
        std::size_t p = 1;
        while (p < n)
            p <<= 1;
        return p;
    }

    // Fibonacci hashing: one multiply, top bits feed the index.
    [[nodiscard]] std::size_t probe_start(std::uint64_t ref) const noexcept {
        return static_cast<std::size_t>((ref * 0x9E3779B97F4A7C15ULL) >> table_shift_);
    }

    struct FindResult {
        std::size_t slot = 0;
        OrderHandle handle{};
    };

    [[nodiscard]] FindResult id_find_slot(std::uint64_t ref) const noexcept {
        MOG_PRE(ref != 0);
        std::size_t i = probe_start(ref);
        while (entries_[i].ref != 0) {
            if (entries_[i].ref == ref)
                return FindResult{i, entries_[i].handle};
            i = (i + 1) & (table_cap_ - 1);
        }
        return FindResult{i, OrderHandle{}};
    }

    [[nodiscard]] OrderHandle id_find(std::uint64_t ref) const noexcept {
        return id_find_slot(ref).handle;
    }

    void id_insert_at(std::size_t slot, std::uint64_t ref, OrderHandle h) noexcept {
        MOG_PRE(ref != 0 && entries_[slot].ref == 0);
        entries_[slot].ref = ref;
        entries_[slot].handle = h;
        ++table_size_;
        MOG_POST(table_size_ * 2 <= table_cap_);
    }

    void id_insert(std::uint64_t ref, OrderHandle h) noexcept {
        const auto res = id_find_slot(ref);
        MOG_PRE(res.handle.index == kNullIndex);
        id_insert_at(res.slot, ref, h);
    }

    // Backward-shift deletion keeps probe chains intact without tombstones.
    void id_erase_at(std::size_t slot) noexcept {
        std::size_t i = slot;
        MOG_PRE(entries_[i].ref != 0);
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & (table_cap_ - 1);
            if (entries_[j].ref == 0)
                break;
            const std::size_t home = probe_start(entries_[j].ref);
            const bool home_in_gap = i < j ? (home > i && home <= j) : (home > i || home <= j);
            if (!home_in_gap) {
                entries_[i] = entries_[j];
                i = j;
            }
        }
        entries_[i].ref = 0;
        --table_size_;
    }

    void id_erase(std::uint64_t ref) noexcept {
        const auto res = id_find_slot(ref);
        MOG_PRE(res.handle.index != kNullIndex);
        id_erase_at(res.slot);
    }

    void push_tail(Level& lv, std::uint32_t idx) noexcept {
        Order& o = arena_.at_index(idx);
        o.prev = lv.tail;
        o.next = kNullIndex;
        if (lv.tail != kNullIndex)
            arena_.at_index(lv.tail).next = idx;
        else
            lv.head = idx;
        lv.tail = idx;
        ++lv.count;
    }

    void unlink(Level& lv, std::uint32_t idx) noexcept {
        Order& o = arena_.at_index(idx);
        if (o.prev != kNullIndex)
            arena_.at_index(o.prev).next = o.next;
        else
            lv.head = o.next;
        if (o.next != kNullIndex)
            arena_.at_index(o.next).prev = o.prev;
        else
            lv.tail = o.prev;
        --lv.count;
    }

    [[nodiscard]] BookTick reduce(OrderHandle h, Order& o, std::int64_t qty,
                                  BookEventKind partial_kind, inplace_vector<LevelDelta, 3>* deltas,
                                  std::size_t slot_hint = kNullIndex) noexcept {
        MOG_PRE(qty > 0 && qty <= o.qty_remaining);
        // Capture before release mutates slot metadata; the reference stays valid
        // memory but its handle goes stale by design.
        const std::uint64_t ref = o.order_ref;
        const std::int64_t ticks = o.price_ticks;
        const Side side = o.side == 'B' ? Side::buy : Side::sell;
        LadderT& lad = ladder_for(side);

        o.qty_remaining -= qty;
        const bool emptied = o.qty_remaining == 0;
        // Single fused walk: apply returns the level, and sticky pages keep it
        // valid for the unlink afterwards. A phantom level (book corruption)
        // drives qty negative and trips the ladder postcondition instead.
        Level* lv = lad.apply(ticks, -qty);
        if (emptied)
            unlink(*lv, h.index);
        if (emptied) {
            arena_.release(h);
            if (slot_hint != kNullIndex)
                id_erase_at(slot_hint);
            else
                id_erase(ref);
        } else {
            MOG_POST(o.qty_remaining > 0);
        }

        if (deltas != nullptr)
            static_cast<void>(
                deltas->emplace_back(LevelDelta{.price_ticks = ticks,
                                                .qty_delta = static_cast<std::int32_t>(-qty),
                                                .side = static_cast<std::uint8_t>(to_wire(side))}));

        const bool level_empty = lv->qty_total == 0;
        MOG_CONTRACT_ASSERT(!level_empty || emptied); // level can only drain via full removal
        if (level_empty) {
            if (side == Side::buy && best_bid_ == ticks)
                best_bid_ = lad.first_below(best_bid_);
            else if (side == Side::sell && best_ask_ == ticks)
                best_ask_ = lad.first_above(best_ask_);
        }
        return BookTick{
            .price_ticks = ticks,
            .qty = static_cast<std::uint32_t>(qty),
            .kind = static_cast<std::uint8_t>(emptied ? BookEventKind::removed : partial_kind),
            .side = static_cast<std::uint8_t>(to_wire(side)),
            .error = 0,
            .reserved = 0};
    }

    [[nodiscard]] bool check_fifos(Side side) const noexcept {
        const LadderT& lad = ladder_for(side);
        const char sc = to_wire(side);
        bool ok = true;
        lad.for_each_level([&](std::int64_t tick, const Level& lv) {
            std::int64_t sum = 0;
            std::uint32_t n = lv.head;
            std::uint32_t walked = 0;
            std::uint32_t prev = kNullIndex;
            while (n != kNullIndex) {
                const Order& o = arena_.at_index(n);
                ok = ok && o.side == sc && o.price_ticks == tick && o.qty_remaining > 0 &&
                     o.prev == prev;
                prev = n;
                sum += o.qty_remaining;
                ++walked;
                n = o.next;
                ok = ok && walked <= arena_.capacity();
            }
            ok = ok && prev == lv.tail && walked == lv.count && sum == lv.qty_total &&
                 lv.qty_total > 0;
        });
        return ok;
    }

    [[nodiscard]] std::int64_t brute_best(Side side) const noexcept {
        const LadderT& lad = ladder_for(side);
        std::int64_t best = kNoTick;
        lad.for_each_level([&](std::int64_t tick, const Level&) {
            if (side == Side::buy) {
                if (best == kNoTick || tick > best)
                    best = tick;
            } else if (best == kNoTick || tick < best) {
                best = tick;
            }
        });
        return best;
    }

    Arena<Order, OrderHandleTag> arena_;
    LadderT bid_ladder_;
    LadderT ask_ladder_;
    std::size_t table_cap_ = 0;
    std::size_t table_shift_ = 0;
    std::size_t table_size_ = 0;
    Entry* entries_ = nullptr;
    std::int64_t best_bid_ = kNoTick;
    std::int64_t best_ask_ = kNoTick;
};

} // namespace mog
