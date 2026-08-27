// Naive std::map reference book mirroring OrderBook semantics op-for-op,
// including validation order and error codes, for differential fuzzing.
#pragma once

#include <mog/OrderBook.hpp>
#include <mog/Types.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace mog::testing {

class ReferenceBook {
public:
    // bids_ uses std::greater<>, so begin() is the highest price.
    [[nodiscard]] std::int64_t best_bid() const noexcept {
        return bids_.empty() ? kNoTick : bids_.begin()->first;
    }
    [[nodiscard]] std::int64_t best_ask() const noexcept {
        return asks_.empty() ? kNoTick : asks_.begin()->first;
    }
    [[nodiscard]] std::size_t live_orders() const noexcept { return orders_.size(); }

    [[nodiscard]] BookTick add(OrderId ref, Side side, Qty qty, Price price,
                               inplace_vector<LevelDelta, 3>* deltas = nullptr) noexcept {
        if (qty.units <= 0)
            return tick_error(BookError::bad_qty);
        if (find(ref.value) != orders_.end())
            return tick_error(BookError::duplicate_ref);
        auto& lvl = side == Side::buy ? bids_[price.ticks] : asks_[price.ticks];
        lvl += qty.units;
        orders_[ref.value] = {to_wire(side), price.ticks, qty.units};
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
            return tick_error(BookError::bad_qty);
        auto it = find(ref.value);
        if (it == orders_.end())
            return tick_error(BookError::unknown_order);
        if (qty.units > std::get<2>(it->second))
            return tick_error(BookError::over_execute);
        return reduce(it, qty.units, deltas);
    }

    [[nodiscard]] BookTick cancel(OrderId ref, Qty qty,
                                  inplace_vector<LevelDelta, 3>* deltas = nullptr) noexcept {
        return execute(ref, qty, deltas);
    }

    [[nodiscard]] BookTick remove(OrderId ref,
                                  inplace_vector<LevelDelta, 3>* deltas = nullptr) noexcept {
        auto it = find(ref.value);
        if (it == orders_.end())
            return tick_error(BookError::unknown_order);
        return reduce(it, std::get<2>(it->second), deltas);
    }

    // Validation precedence mirrors OrderBook::replace exactly.
    [[nodiscard]] BookTick replace(OrderId orig_ref, OrderId new_ref, Qty new_qty, Price new_price,
                                   inplace_vector<LevelDelta, 3>* deltas = nullptr) noexcept {
        if (new_qty.units <= 0)
            return tick_error(BookError::bad_qty);
        auto orig = find(orig_ref.value);
        if (orig == orders_.end())
            return tick_error(BookError::unknown_order);
        if (orig_ref.value != new_ref.value && find(new_ref.value) != orders_.end())
            return tick_error(BookError::duplicate_ref);

        const Side orig_side = std::get<0>(orig->second) == 'B' ? Side::buy : Side::sell;
        const std::int64_t orig_qty = std::get<2>(orig->second);
        inplace_vector<LevelDelta, 3> staged{};
        const BookTick removed = reduce(orig, orig_qty, &staged);
        MOG_CONTRACT_ASSERT(ok(removed));
        if (deltas != nullptr)
            for (const LevelDelta& d : staged)
                static_cast<void>(deltas->push_back(d)); // removal precedes addition
        const BookTick added = add(new_ref, orig_side, new_qty, new_price, deltas);
        MOG_CONTRACT_ASSERT(ok(added));
        return BookTick{.price_ticks = added.price_ticks,
                        .qty = added.qty,
                        .kind = static_cast<std::uint8_t>(BookEventKind::replaced),
                        .side = added.side,
                        .error = 0,
                        .reserved = 0};
    }

    template <class Fn>
    void for_each_l2(Fn&& fn) const noexcept {
        for (const auto& [tick, qty] : bids_)
            fn(Side::buy, tick, qty);
        for (const auto& [tick, qty] : asks_)
            fn(Side::sell, tick, qty);
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
            return tick_error(BookError::unknown_order);
        }
    }

private:
    using MapT = std::unordered_map<std::uint64_t, std::tuple<char, std::int64_t, std::int64_t>>;

    [[nodiscard]] static BookTick tick_error(BookError e) noexcept {
        return BookTick{.price_ticks = 0,
                        .qty = 0,
                        .kind = kTickError,
                        .side = 0,
                        .error = static_cast<std::uint8_t>(e),
                        .reserved = 0};
    }

    [[nodiscard]] typename MapT::iterator find(std::uint64_t ref) noexcept {
        return orders_.find(ref);
    }

    [[nodiscard]] BookTick reduce(typename MapT::iterator it, std::int64_t qty,
                                  inplace_vector<LevelDelta, 3>* deltas) noexcept {
        const char side_c = std::get<0>(it->second);
        const std::int64_t ticks = std::get<1>(it->second);
        const std::int64_t remaining = std::get<2>(it->second);
        const Side side = side_c == 'B' ? Side::buy : Side::sell;
        const bool emptied = qty == remaining;
        if (side == Side::buy)
            apply_level(bids_, ticks, -qty);
        else
            apply_level(asks_, ticks, -qty);
        if (emptied) {
            orders_.erase(it);
        } else {
            std::get<2>(it->second) -= qty;
            MOG_CONTRACT_ASSERT(std::get<2>(it->second) > 0);
        }

        if (deltas != nullptr)
            static_cast<void>(
                deltas->emplace_back(LevelDelta{.price_ticks = ticks,
                                                .qty_delta = static_cast<std::int32_t>(-qty),
                                                .side = static_cast<std::uint8_t>(to_wire(side))}));
        return BookTick{.price_ticks = ticks,
                        .qty = static_cast<std::uint32_t>(qty),
                        .kind = static_cast<std::uint8_t>(emptied ? BookEventKind::removed
                                                                  : BookEventKind::reduced),
                        .side = static_cast<std::uint8_t>(to_wire(side)),
                        .error = 0,
                        .reserved = 0};
    }

    template <class MapT2>
    static void apply_level(MapT2& m, std::int64_t tick, std::int64_t delta) noexcept {
        auto it = m.find(tick);
        MOG_CONTRACT_ASSERT(it != m.end());
        it->second += delta;
        if (it->second == 0)
            m.erase(it);
        else
            MOG_CONTRACT_ASSERT(it->second > 0);
    }

    std::map<std::int64_t, std::int64_t, std::greater<>> bids_;
    std::map<std::int64_t, std::int64_t> asks_;
    MapT orders_;
};

} // namespace mog::testing
