// mog._core - thin nanobind shim over the public mog API.
// Doctrine: bindings forward to C++; no logic lives here. Every function is
// a direct call into an existing public entry point with expected/error ->
// Python exception translation.
#include <mog/Build.hpp>
#include <mog/Contracts.hpp>
#include <mog/ITCHParser.hpp>
#include <mog/Orchestrate.hpp>
#include <mog/OrderBook.hpp>
#include <mog/Sha256.hpp>
#include <mog/Simulate.hpp>
#include <mog/TimeTravel.hpp>
#include <mog/Types.hpp>

#include <support/CorpusGen.hpp> // deterministic corpus generation (header-only)

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace nb = nanobind;
using namespace mog;

namespace {

struct MogDecodeError : std::runtime_error {
    explicit MogDecodeError(DecodeError err_code)
        : std::runtime_error(std::string("decode error: ") + std::string(error_text(err_code))),
          code(err_code) {}
    DecodeError code;
};

struct MogParseError : std::runtime_error {
    MogParseError(std::size_t off, DecodeError err_code)
        : std::runtime_error("parse error at byte " + std::to_string(off) + ": " +
                             std::string(error_text(err_code))),
          offset(off), code(err_code) {}
    std::size_t offset;
    DecodeError code;
};

Side side_arg(char c) {
    if (c != 'B' && c != 'S')
        throw std::runtime_error("side must be 'B' or 'S'");
    return side_from_wire(c);
}

// Union-member access guard: throws unless `type` matches the active member.
void require(const Message& m, const char* field, std::initializer_list<char> types) {
    for (const char t : types)
        if (m.type == t)
            return;
    std::string want;
    for (const char t : types) {
        if (!want.empty())
            want += '/';
        want += t;
    }
    throw std::runtime_error(std::string("field '") + field + "' requires message type " + want);
}

// The wire header is field-identical across all seven members; read it
// through whichever member is active to stay inside union rules.
const Header& hdr(const Message& m) {
    switch (m.type) {
    case 'F':
        return m.add_order_attribution.header;
    case 'E':
        return m.order_executed.header;
    case 'C':
        return m.order_executed_with_price.header;
    case 'X':
        return m.order_cancel.header;
    case 'D':
        return m.order_delete.header;
    case 'U':
        return m.order_replace.header;
    default:
        return m.add_order.header;
    }
}

std::uint64_t order_ref_of(const Message& m) {
    require(m, "order_ref", {'A', 'F', 'E', 'C', 'X', 'D'});
    switch (m.type) {
    case 'E':
    case 'C':
        return m.order_executed.order_ref.value;
    case 'X':
        return m.order_cancel.order_ref.value;
    case 'D':
        return m.order_delete.order_ref.value;
    default:
        return m.add_order.order_ref.value;
    }
}

struct PySink {
    nb::callable fn;
    void on_message(const Message& m) { fn(m); }
};

PriceLadder<>::Config ladder_config(std::int64_t lo, std::int64_t hi, std::size_t pool) {
    return PriceLadder<>::Config{lo, hi, pool};
}

std::array<unsigned char, 32> digest_of(const ExecutionSimulator& sim) {
    return sim.trace_digest();
}

// Shared config assembly for ExecutionSimulator and TimeTravelSession so
// both bindings accept identical constructor arguments and defaults.
SimConfig make_sim_config(std::size_t arena_capacity, std::int64_t lo_tick, std::int64_t hi_tick,
                          std::size_t page_pool, std::size_t event_capacity, std::uint64_t seed,
                          double bid_depletion_per_us, double ask_depletion_per_us,
                          double hawkes_kappa, std::uint64_t hawkes_decay_ns, double momentum_gain,
                          std::uint64_t momentum_memory_ns, std::uint32_t parse_latency_ns,
                          std::uint32_t decision_latency_ns, std::uint32_t wire_latency_ns,
                          std::uint32_t jitter_max_ns, JitterKind jitter_kind,
                          double jitter_mean_ns, double jitter_sigma_ns,
                          std::uint64_t external_ref_limit, std::int64_t maker_fee_bps,
                          std::int64_t taker_fee_bps) {
    SimConfig cfg{};
    cfg.book.arena_capacity = arena_capacity;
    cfg.book.ladder = ladder_config(lo_tick, hi_tick, page_pool);
    cfg.event_capacity = event_capacity;
    cfg.seed = seed;
    cfg.bid_depletion_per_us = bid_depletion_per_us;
    cfg.ask_depletion_per_us = ask_depletion_per_us;
    cfg.hawkes_kappa = hawkes_kappa;
    cfg.hawkes_decay_ns = hawkes_decay_ns;
    cfg.momentum_gain = momentum_gain;
    cfg.momentum_memory_ns = momentum_memory_ns;
    cfg.parse_latency_ns = parse_latency_ns;
    cfg.decision_latency_ns = decision_latency_ns;
    cfg.wire_latency_ns = wire_latency_ns;
    cfg.jitter_max_ns = jitter_max_ns;
    cfg.jitter_kind = jitter_kind;
    cfg.jitter_mean_ns = jitter_mean_ns;
    cfg.jitter_sigma_ns = jitter_sigma_ns;
    cfg.external_ref_limit = external_ref_limit;
    cfg.maker_fee_bps = maker_fee_bps;
    cfg.taker_fee_bps = taker_fee_bps;
    return cfg;
}

template <std::size_t N>
std::string to_hex(const unsigned char (&raw)[N]) {
    std::string out(N * 2, '0');
    static constexpr char kDigits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < N; ++i) {
        out[i * 2] = kDigits[raw[i] >> 4];
        out[i * 2 + 1] = kDigits[raw[i] & 0x0F];
    }
    return out;
}

} // namespace

NB_MODULE(_core, m) {
    m.attr("__version__") = MOG_VERSION;

    nb::exception<MogDecodeError>(m, "MogDecodeError");
    nb::exception<MogParseError>(m, "MogParseError");

    // --- messages ----------------------------------------------------------
    nb::class_<Message>(m, "Message")
        .def_prop_ro("type", [](const Message& x) { return std::string(1, x.type); })
        .def_prop_ro("locate", [](const Message& x) { return hdr(x).locate.value; })
        .def_prop_ro("tracking", [](const Message& x) { return hdr(x).tracking.value; })
        .def_prop_ro("ts_ns", [](const Message& x) { return hdr(x).ts_ns.value; })
        .def_prop_ro("order_ref", [](const Message& x) { return order_ref_of(x); })
        .def_prop_ro("side",
                     [](const Message& x) {
                         require(x, "side", {'A', 'F'});
                         return std::string(1, to_wire(x.add_order.side));
                     })
        .def_prop_ro("shares",
                     [](const Message& x) {
                         require(x, "shares", {'A', 'F', 'U'});
                         return x.type == 'U' ? x.order_replace.shares.units
                                              : x.add_order.shares.units;
                     })
        .def_prop_ro("stock",
                     [](const Message& x) {
                         require(x, "stock", {'A', 'F'});
                         return std::string(symbol_view(x.add_order.stock));
                     })
        .def_prop_ro("price",
                     [](const Message& x) {
                         require(x, "price", {'A', 'F', 'U'});
                         return x.type == 'U' ? x.order_replace.price.ticks
                                              : x.add_order.price.ticks;
                     })
        .def_prop_ro("attribution",
                     [](const Message& x) {
                         require(x, "attribution", {'F'});
                         return std::string(x.add_order_attribution.attribution.data(), 4);
                     })
        .def_prop_ro("executed_shares",
                     [](const Message& x) {
                         require(x, "executed_shares", {'E', 'C'});
                         return x.type == 'C' ? x.order_executed_with_price.executed_shares.units
                                              : x.order_executed.executed_shares.units;
                     })
        .def_prop_ro("match_number",
                     [](const Message& x) {
                         require(x, "match_number", {'E', 'C'});
                         return x.type == 'C' ? x.order_executed_with_price.match_number.value
                                              : x.order_executed.match_number.value;
                     })
        .def_prop_ro("printable",
                     [](const Message& x) {
                         require(x, "printable", {'C'});
                         return x.order_executed_with_price.printable;
                     })
        .def_prop_ro("execution_price",
                     [](const Message& x) {
                         require(x, "execution_price", {'C'});
                         return x.order_executed_with_price.execution_price.ticks;
                     })
        .def_prop_ro("cancelled_shares",
                     [](const Message& x) {
                         require(x, "cancelled_shares", {'X'});
                         return x.order_cancel.cancelled_shares.units;
                     })
        .def_prop_ro("original_order_ref",
                     [](const Message& x) {
                         require(x, "original_order_ref", {'U'});
                         return x.order_replace.original_order_ref.value;
                     })
        .def_prop_ro("new_order_ref",
                     [](const Message& x) {
                         require(x, "new_order_ref", {'U'});
                         return x.order_replace.new_order_ref.value;
                     })
        .def("__repr__",
             [](const Message& x) { return std::string("<mog.Message '") + x.type + "'>"; });

    m.def(
        "decode_message",
        [](nb::bytes raw) -> Message {
            auto r =
                decode_message(reinterpret_cast<const unsigned char*>(raw.c_str()), raw.size());
            if (!r)
                throw MogDecodeError(r.error());
            return *r;
        },
        nb::arg("buf"), "Decode one wire record into a Message.");

    m.def(
        "frame_length",
        [](char type) -> std::optional<int> {
            const std::size_t len = kFrame.length[static_cast<unsigned char>(type)];
            if (len == 0)
                return std::nullopt;
            return static_cast<int>(len);
        },
        nb::arg("type"), "Wire record length for a message type char, or None.");

    m.def(
        "parse_itch",
        [](nb::bytes raw, nb::callable sink) {
            PySink holder{sink};
            const auto* data = reinterpret_cast<const unsigned char*>(raw.c_str());
            auto r = parse_itch(data, raw.size(), holder);
            if (!r)
                throw MogParseError(r.error().offset, r.error().code);
            return *r;
        },
        nb::arg("buf"), nb::arg("sink"),
        "Parse an ITCH capture, calling sink(Message) per record. "
        "Returns bytes consumed.");

    m.def(
        "trace_hash", [](const Message& x, std::uint64_t seed) { return trace_hash(x, seed); },
        nb::arg("msg"), nb::arg("seed"), "FNV-1a fold of decoded fields.");

    // --- order book --------------------------------------------------------
    nb::class_<BookTick>(m, "BookTick")
        .def_ro("price_ticks", &BookTick::price_ticks)
        .def_ro("qty", &BookTick::qty)
        .def_ro("kind", &BookTick::kind)
        .def_prop_ro("side",
                     [](const BookTick& t) { return std::string(1, static_cast<char>(t.side)); })
        .def_ro("error", &BookTick::error)
        .def_prop_ro("ok", [](const BookTick& t) { return ok(t); });

    nb::class_<OrderBook<>>(m, "OrderBook")
        .def(
            "__init__",
            [](OrderBook<>* t, std::size_t arena_capacity, std::int64_t lo_tick,
               std::int64_t hi_tick, std::size_t page_pool) {
                new (t) OrderBook<>(OrderBook<>::Config{
                    arena_capacity, ladder_config(lo_tick, hi_tick, page_pool)});
            },
            nb::arg("arena_capacity") = 1 << 20, nb::arg("lo_tick"), nb::arg("hi_tick"),
            nb::arg("page_pool") = 512)
        .def(
            "add",
            [](OrderBook<>& b, std::uint64_t ref, char side, std::int64_t qty, std::int64_t price) {
                return b.add(OrderId{ref}, side_arg(side), Qty{qty}, Price{price});
            })
        .def("execute", [](OrderBook<>& b, std::uint64_t ref,
                           std::int64_t qty) { return b.execute(OrderId{ref}, Qty{qty}); })
        .def("cancel", [](OrderBook<>& b, std::uint64_t ref,
                          std::int64_t qty) { return b.cancel(OrderId{ref}, Qty{qty}); })
        .def("remove", [](OrderBook<>& b, std::uint64_t ref) { return b.remove(OrderId{ref}); })
        .def("replace",
             [](OrderBook<>& b, std::uint64_t orig, std::uint64_t fresh, std::int64_t qty,
                std::int64_t price) {
                 return b.replace(OrderId{orig}, OrderId{fresh}, Qty{qty}, Price{price});
             })
        .def(
            "apply", [](OrderBook<>& b, const Message& msg) { return b.apply(msg); },
            nb::arg("msg"), "Route one decoded message ('A','F','E','C','X','D','U').")
        .def("best_bid",
             [](const OrderBook<>& b) -> std::optional<std::int64_t> {
                 if (b.best_bid() == kNoTick)
                     return std::nullopt;
                 return b.best_bid();
             })
        .def("best_ask",
             [](const OrderBook<>& b) -> std::optional<std::int64_t> {
                 if (b.best_ask() == kNoTick)
                     return std::nullopt;
                 return b.best_ask();
             })
        .def("live_orders", [](const OrderBook<>& b) { return b.live_orders(); })
        .def("qty_at", [](const OrderBook<>& b, char side,
                          std::int64_t price) { return b.qty_at(side_arg(side), Price{price}); })
        .def("remaining_of",
             [](const OrderBook<>& b, std::uint64_t ref) { return b.remaining_of(OrderId{ref}); })
        .def("in_band", [](const OrderBook<>& b, std::int64_t tick) { return b.in_band(tick); })
        .def("audit", [](const OrderBook<>& b) { return b.audit(); })
        .def(
            "l2",
            [](const OrderBook<>& b) {
                std::vector<std::tuple<char, std::int64_t, std::int64_t>> rows;
                b.for_each_l2([&](Side s, std::int64_t tick, std::int64_t qty) {
                    rows.emplace_back(to_wire(s), tick, qty);
                });
                return rows;
            },
            "L2 snapshot as (side, price_ticks, qty_total) tuples.");

    // --- execution simulator ----------------------------------------------
    nb::enum_<SimOrderType>(m, "SimOrderType")
        .value("day_limit", SimOrderType::day_limit)
        .value("market", SimOrderType::market)
        .value("ioc", SimOrderType::ioc)
        .value("post_only", SimOrderType::post_only);

    nb::enum_<StpMode>(m, "StpMode")
        .value("none", StpMode::none)
        .value("cancel_newest", StpMode::cancel_newest)
        .value("cancel_oldest", StpMode::cancel_oldest)
        .value("decrement", StpMode::decrement);

    nb::enum_<JitterKind>(m, "JitterKind")
        .value("none", JitterKind::none)
        .value("uniform", JitterKind::uniform)
        .value("exponential", JitterKind::exponential)
        .value("normal", JitterKind::normal);

    nb::class_<SimFillReport>(m, "FillReport")
        .def_ro("visible_ts", &SimFillReport::visible_ts)
        .def_ro("ref", &SimFillReport::ref)
        .def_ro("price_ticks", &SimFillReport::price_ticks)
        .def_ro("qty", &SimFillReport::qty)
        .def_ro("seq", &SimFillReport::seq)
        .def_ro("fee", &SimFillReport::fee)
        .def_prop_ro("side", [](const SimFillReport& r) { return std::string(1, r.side); });

    nb::class_<SimTrade>(m, "Trade")
        .def_ro("ts", &SimTrade::ts)
        .def_ro("seq", &SimTrade::seq)
        .def_ro("px_ticks", &SimTrade::px_ticks)
        .def_ro("qty", &SimTrade::qty)
        .def_ro("aggressor_buy", &SimTrade::aggressor_buy);

    nb::class_<SimDecision>(m, "Decision")
        .def_prop_ro("kind", [](const SimDecision& d) { return static_cast<int>(d.kind); })
        .def_ro("filled_qty", &SimDecision::filled_qty)
        .def_ro("cancelled_qty", &SimDecision::cancelled_qty)
        .def_ro("filled_notional", &SimDecision::filled_notional)
        .def_ro("visible_ts", &SimDecision::visible_ts)
        .def_ro("ref", &SimDecision::ref)
        .def_prop_ro("side", [](const SimDecision& d) { return std::string(1, d.side); })
        .def_ro("seq", &SimDecision::seq)
        .def_ro("fee", &SimDecision::fee);

    nb::class_<ExecutionSimulator>(m, "ExecutionSimulator")
        .def(
            "__init__",
            [](ExecutionSimulator* t, std::size_t arena_capacity, std::int64_t lo_tick,
               std::int64_t hi_tick, std::size_t page_pool, std::size_t event_capacity,
               std::uint64_t seed, double bid_depletion_per_us, double ask_depletion_per_us,
               double hawkes_kappa, std::uint64_t hawkes_decay_ns, double momentum_gain,
               std::uint64_t momentum_memory_ns, std::uint32_t parse_latency_ns,
               std::uint32_t decision_latency_ns, std::uint32_t wire_latency_ns,
               std::uint32_t jitter_max_ns, JitterKind jitter_kind, double jitter_mean_ns,
               double jitter_sigma_ns, std::uint64_t external_ref_limit, std::int64_t maker_fee_bps,
               std::int64_t taker_fee_bps) {
                new (t) ExecutionSimulator(make_sim_config(
                    arena_capacity, lo_tick, hi_tick, page_pool, event_capacity, seed,
                    bid_depletion_per_us, ask_depletion_per_us, hawkes_kappa, hawkes_decay_ns,
                    momentum_gain, momentum_memory_ns, parse_latency_ns, decision_latency_ns,
                    wire_latency_ns, jitter_max_ns, jitter_kind, jitter_mean_ns, jitter_sigma_ns,
                    external_ref_limit, maker_fee_bps, taker_fee_bps));
            },
            nb::arg("arena_capacity") = 1 << 20, nb::arg("lo_tick"), nb::arg("hi_tick"),
            nb::arg("page_pool") = 512, nb::arg("event_capacity") = 1 << 20, nb::arg("seed") = 1,
            nb::arg("bid_depletion_per_us") = 0.0, nb::arg("ask_depletion_per_us") = 0.0,
            nb::arg("hawkes_kappa") = 0.0, nb::arg("hawkes_decay_ns") = 0,
            nb::arg("momentum_gain") = 0.0, nb::arg("momentum_memory_ns") = 0,
            nb::arg("parse_latency_ns") = 0, nb::arg("decision_latency_ns") = 0,
            nb::arg("wire_latency_ns") = 0, nb::arg("jitter_max_ns") = 0,
            nb::arg("jitter_kind") = JitterKind::none, nb::arg("jitter_mean_ns") = 0.0,
            nb::arg("jitter_sigma_ns") = 0.0,
            nb::arg("external_ref_limit") = std::uint64_t{1} << 62, nb::arg("maker_fee_bps") = 0,
            nb::arg("taker_fee_bps") = 0)
        .def("seed_external",
             [](ExecutionSimulator& s, std::uint64_t ref, char side, std::int64_t qty,
                std::int64_t price) {
                 return s.seed_external(OrderId{ref}, side_arg(side), Qty{qty}, Price{price});
             })
        .def("seed_iceberg",
             [](ExecutionSimulator& s, std::uint64_t ref, char side, std::int64_t price,
                std::int64_t display, std::int64_t total) {
                 IcebergSpec spec{};
                 spec.ref = OrderId{ref};
                 spec.side = side_arg(side);
                 spec.price = Price{price};
                 spec.display = display;
                 spec.total = total;
                 return s.seed_iceberg(spec);
             })
        .def("iceberg_hidden", [](ExecutionSimulator& s,
                                  std::uint64_t ref) { return s.iceberg_hidden(OrderId{ref}); })
        .def(
            "submit",
            [](ExecutionSimulator& s, std::uint64_t ref, char side, std::int64_t qty,
               std::int64_t price, SimOrderType type, std::uint64_t arrival_ts) {
                return s.submit(
                    SimInbound{OrderId{ref}, side_arg(side), Qty{qty}, Price{price}, type},
                    arrival_ts);
            },
            nb::arg("ref"), nb::arg("side"), nb::arg("qty"), nb::arg("price"), nb::arg("type"),
            nb::arg("arrival_ts"))
        .def("run_until",
             [](ExecutionSimulator& s, std::uint64_t until_ts) { s.run_until(until_ts); })
        .def("drain", [](ExecutionSimulator& s) { s.drain(); })
        .def("apply_external",
             [](ExecutionSimulator& s, char side, std::int64_t price, std::int64_t qty) {
                 s.apply_external(side_arg(side), Price{price}, qty);
             })
        .def("advance_time",
             [](ExecutionSimulator& s, std::uint64_t dt_ns) { s.advance_time(dt_ns); })
        .def("flow_excitement", [](const ExecutionSimulator& s) { return s.flow_excitement(); })
        .def("flow_momentum", [](const ExecutionSimulator& s) { return s.flow_momentum(); })
        .def("cancel_strategy", [](ExecutionSimulator& s,
                                   std::uint64_t ref) { return s.cancel_strategy(OrderId{ref}); })
        .def("replace_strategy",
             [](ExecutionSimulator& s, std::uint64_t orig, std::uint64_t fresh, std::int64_t qty,
                std::int64_t price) {
                 return s.replace_strategy(OrderId{orig}, OrderId{fresh}, Qty{qty}, Price{price});
             })
        .def("pending_decisions", [](const ExecutionSimulator& s) { return s.pending_decisions(); })
        .def("queue_ahead_of", [](const ExecutionSimulator& s,
                                  std::uint64_t ref) { return s.queue_ahead_of(OrderId{ref}); })
        .def("reports", [](const ExecutionSimulator& s) { return s.reports(); } /* copy */)
        .def("trades", [](const ExecutionSimulator& s) { return s.trades(); } /* copy */)
        .def("decisions", [](const ExecutionSimulator& s) { return s.decisions(); } /* copy */)
        .def(
            "trace_digest",
            [](const ExecutionSimulator& s) {
                char hex[65];
                static_cast<void>(Sha256::hex(digest_of(s), hex));
                return std::string(hex, 64);
            },
            "SHA-256 over fills+decisions; deterministic given identical scripts.")
        .def("set_stp_mode", [](ExecutionSimulator& s, StpMode mode) { s.set_stp_mode(mode); })
        .def("audit", [](const ExecutionSimulator& s) { return s.audit(); })
        .def(
            "book", [](const ExecutionSimulator& s) -> const OrderBook<>& { return s.book(); },
            nb::rv_policy::reference_internal);

    // --- time-travel debugger ----------------------------------------------
    nb::class_<TimeTravelSession>(m, "TimeTravelSession")
        .def(
            "__init__",
            [](TimeTravelSession* t, std::size_t arena_capacity, std::int64_t lo_tick,
               std::int64_t hi_tick, std::size_t page_pool, std::size_t event_capacity,
               std::uint64_t seed, double bid_depletion_per_us, double ask_depletion_per_us,
               double hawkes_kappa, std::uint64_t hawkes_decay_ns, double momentum_gain,
               std::uint64_t momentum_memory_ns, std::uint32_t parse_latency_ns,
               std::uint32_t decision_latency_ns, std::uint32_t wire_latency_ns,
               std::uint32_t jitter_max_ns, JitterKind jitter_kind, double jitter_mean_ns,
               double jitter_sigma_ns, std::uint64_t external_ref_limit, std::int64_t maker_fee_bps,
               std::int64_t taker_fee_bps) {
                new (t) TimeTravelSession(make_sim_config(
                    arena_capacity, lo_tick, hi_tick, page_pool, event_capacity, seed,
                    bid_depletion_per_us, ask_depletion_per_us, hawkes_kappa, hawkes_decay_ns,
                    momentum_gain, momentum_memory_ns, parse_latency_ns, decision_latency_ns,
                    wire_latency_ns, jitter_max_ns, jitter_kind, jitter_mean_ns, jitter_sigma_ns,
                    external_ref_limit, maker_fee_bps, taker_fee_bps));
            },
            nb::arg("arena_capacity") = 1 << 20, nb::arg("lo_tick"), nb::arg("hi_tick"),
            nb::arg("page_pool") = 512, nb::arg("event_capacity") = 1 << 20, nb::arg("seed") = 1,
            nb::arg("bid_depletion_per_us") = 0.0, nb::arg("ask_depletion_per_us") = 0.0,
            nb::arg("hawkes_kappa") = 0.0, nb::arg("hawkes_decay_ns") = 0,
            nb::arg("momentum_gain") = 0.0, nb::arg("momentum_memory_ns") = 0,
            nb::arg("parse_latency_ns") = 0, nb::arg("decision_latency_ns") = 0,
            nb::arg("wire_latency_ns") = 0, nb::arg("jitter_max_ns") = 0,
            nb::arg("jitter_kind") = JitterKind::none, nb::arg("jitter_mean_ns") = 0.0,
            nb::arg("jitter_sigma_ns") = 0.0,
            nb::arg("external_ref_limit") = std::uint64_t{1} << 62, nb::arg("maker_fee_bps") = 0,
            nb::arg("taker_fee_bps") = 0)
        .def("seed_external",
             [](TimeTravelSession& s, std::uint64_t ref, char side, std::int64_t qty,
                std::int64_t price) {
                 return s.seed_external(OrderId{ref}, side_arg(side), Qty{qty}, Price{price});
             })
        .def("seed_iceberg",
             [](TimeTravelSession& s, std::uint64_t ref, char side, std::int64_t price,
                std::int64_t display, std::int64_t total) {
                 IcebergSpec spec{};
                 spec.ref = OrderId{ref};
                 spec.side = side_arg(side);
                 spec.price = Price{price};
                 spec.display = display;
                 spec.total = total;
                 return s.seed_iceberg(spec);
             })
        .def("set_stp_mode", [](TimeTravelSession& s, StpMode mode) { s.set_stp_mode(mode); })
        .def(
            "submit",
            [](TimeTravelSession& s, std::uint64_t ref, char side, std::int64_t qty,
               std::int64_t price, SimOrderType type, std::uint64_t arrival_ts) {
                return s.submit(
                    SimInbound{OrderId{ref}, side_arg(side), Qty{qty}, Price{price}, type},
                    arrival_ts);
            },
            nb::arg("ref"), nb::arg("side"), nb::arg("qty"), nb::arg("price"), nb::arg("type"),
            nb::arg("arrival_ts"))
        .def("run_until",
             [](TimeTravelSession& s, std::uint64_t until_ts) { s.run_until(until_ts); })
        .def("drain", [](TimeTravelSession& s) { s.drain(); })
        .def("apply_external",
             [](TimeTravelSession& s, char side, std::int64_t price, std::int64_t qty) {
                 s.apply_external(side_arg(side), Price{price}, qty);
             })
        .def("advance_time",
             [](TimeTravelSession& s, std::uint64_t dt_ns) { s.advance_time(dt_ns); })
        .def("cancel_strategy", [](TimeTravelSession& s,
                                   std::uint64_t ref) { return s.cancel_strategy(OrderId{ref}); })
        .def("replace_strategy",
             [](TimeTravelSession& s, std::uint64_t orig, std::uint64_t fresh, std::int64_t qty,
                std::int64_t price) {
                 return s.replace_strategy(OrderId{orig}, OrderId{fresh}, Qty{qty}, Price{price});
             })
        .def("cursor", [](const TimeTravelSession& s) { return s.cursor(); })
        .def("op_count", [](const TimeTravelSession& s) { return s.op_count(); })
        .def("op_names",
             [](const TimeTravelSession& s) {
                 std::vector<std::string> names;
                 for (const SimOp& op : s.log())
                     names.emplace_back(op_kind_name(op.kind));
                 return names;
             })
        .def("seek", [](TimeTravelSession& s, std::size_t k) { s.seek(k); })
        .def(
            "fork_at",
            [](const TimeTravelSession& s, std::size_t k) {
                auto* branch = new TimeTravelSession(s.config());
                s.fork_at(k, *branch);
                return branch;
            },
            nb::rv_policy::take_ownership,
            "Independent counterfactual branch over the shared op prefix [0..k).")
        .def(
            "sim", [](const TimeTravelSession& s) -> const ExecutionSimulator& { return s.sim(); },
            nb::rv_policy::reference_internal,
            "Live simulator state; inspect with the ExecutionSimulator API.")
        .def("queue_ahead_of", [](const TimeTravelSession& s,
                                  std::uint64_t ref) { return s.queue_ahead_of(OrderId{ref}); })
        .def("l2_rows",
             [](const TimeTravelSession& s) {
                 std::vector<std::tuple<char, std::int64_t, std::int64_t>> rows;
                 for (const L2Row& r : s.l2_rows())
                     rows.emplace_back(r.side, r.tick, r.qty);
                 return rows;
             })
        .def("audit", [](const TimeTravelSession& s) { return s.audit(); })
        .def(
            "export_report", [](const TimeTravelSession& s) { return s.export_report(); },
            "Deterministic markdown report; identical logs render identically.");

    m.def(
        "first_decision_divergence",
        [](const TimeTravelSession& a, const TimeTravelSession& b) {
            const std::size_t i = TimeTravelSession::first_decision_divergence(a, b);
            if (i == mog::kNoDivergence)
                return std::ptrdiff_t{-1}; // sentinel: no divergence
            return static_cast<std::ptrdiff_t>(i);
        },
        "Index of the first differing decision between two sessions, or -1.");

    // --- multi-instrument orchestration -------------------------------------
    nb::class_<Orchestrator>(m, "Orchestrator")
        .def(nb::init<>())
        .def(
            "add_instrument",
            [](Orchestrator& o, std::size_t arena_capacity, std::int64_t lo_tick,
               std::int64_t hi_tick, std::size_t page_pool, std::size_t event_capacity,
               std::uint64_t seed, double bid_depletion_per_us, double ask_depletion_per_us,
               double hawkes_kappa, std::uint64_t hawkes_decay_ns, double momentum_gain,
               std::uint64_t momentum_memory_ns, std::uint32_t parse_latency_ns,
               std::uint32_t decision_latency_ns, std::uint32_t wire_latency_ns,
               std::uint32_t jitter_max_ns, JitterKind jitter_kind, double jitter_mean_ns,
               double jitter_sigma_ns, std::uint64_t external_ref_limit, std::int64_t maker_fee_bps,
               std::int64_t taker_fee_bps, std::string name) {
                return o.add_instrument(
                    make_sim_config(arena_capacity, lo_tick, hi_tick, page_pool, event_capacity,
                                    seed, bid_depletion_per_us, ask_depletion_per_us, hawkes_kappa,
                                    hawkes_decay_ns, momentum_gain, momentum_memory_ns,
                                    parse_latency_ns, decision_latency_ns, wire_latency_ns,
                                    jitter_max_ns, jitter_kind, jitter_mean_ns, jitter_sigma_ns,
                                    external_ref_limit, maker_fee_bps, taker_fee_bps),
                    std::move(name));
            },
            nb::arg("arena_capacity") = 1 << 20, nb::arg("lo_tick"), nb::arg("hi_tick"),
            nb::arg("page_pool") = 512, nb::arg("event_capacity") = 1 << 20, nb::arg("seed") = 1,
            nb::arg("bid_depletion_per_us") = 0.0, nb::arg("ask_depletion_per_us") = 0.0,
            nb::arg("hawkes_kappa") = 0.0, nb::arg("hawkes_decay_ns") = 0,
            nb::arg("momentum_gain") = 0.0, nb::arg("momentum_memory_ns") = 0,
            nb::arg("parse_latency_ns") = 0, nb::arg("decision_latency_ns") = 0,
            nb::arg("wire_latency_ns") = 0, nb::arg("jitter_max_ns") = 0,
            nb::arg("jitter_kind") = JitterKind::none, nb::arg("jitter_mean_ns") = 0.0,
            nb::arg("jitter_sigma_ns") = 0.0,
            nb::arg("external_ref_limit") = std::uint64_t{1} << 62, nb::arg("maker_fee_bps") = 0,
            nb::arg("taker_fee_bps") = 0, nb::arg("name") = "",
            "Adds an instrument; returns its routing index.")
        .def("count", [](const Orchestrator& o) { return o.count(); })
        .def(
            "name",
            [](const Orchestrator& o, std::size_t idx) -> const std::string& {
                return o.name(idx);
            },
            nb::rv_policy::reference_internal)
        .def(
            "seed_external",
            [](Orchestrator& o, std::size_t idx, std::uint64_t ref, char side, std::int64_t qty,
               std::int64_t price) {
                return o.seed_external(idx, OrderId{ref}, side_arg(side), Qty{qty}, Price{price});
            },
            nb::arg("idx"), nb::arg("ref"), nb::arg("side"), nb::arg("qty"), nb::arg("price"))
        .def(
            "seed_iceberg",
            [](Orchestrator& o, std::size_t idx, std::uint64_t ref, char side, std::int64_t price,
               std::int64_t display, std::int64_t total) {
                IcebergSpec spec{};
                spec.ref = OrderId{ref};
                spec.side = side_arg(side);
                spec.price = Price{price};
                spec.display = display;
                spec.total = total;
                return o.seed_iceberg(idx, spec);
            },
            nb::arg("idx"), nb::arg("ref"), nb::arg("side"), nb::arg("price"), nb::arg("display"),
            nb::arg("total"))
        .def("set_stp_mode",
             [](Orchestrator& o, std::size_t idx, StpMode mode) { o.set_stp_mode(idx, mode); })
        .def(
            "submit",
            [](Orchestrator& o, std::size_t idx, std::uint64_t ref, char side, std::int64_t qty,
               std::int64_t price, SimOrderType type, std::uint64_t arrival_ts) {
                return o.submit(
                    idx, SimInbound{OrderId{ref}, side_arg(side), Qty{qty}, Price{price}, type},
                    arrival_ts);
            },
            nb::arg("idx"), nb::arg("ref"), nb::arg("side"), nb::arg("qty"), nb::arg("price"),
            nb::arg("type"), nb::arg("arrival_ts"))
        .def("run_until", [](Orchestrator& o, std::size_t idx,
                             std::uint64_t until_ts) { o.run_until(idx, until_ts); })
        .def("drain", [](Orchestrator& o, std::size_t idx) { o.drain(idx); })
        .def(
            "apply_external",
            [](Orchestrator& o, std::size_t idx, char side, std::int64_t price, std::int64_t qty) {
                o.apply_external(idx, side_arg(side), Price{price}, qty);
            },
            nb::arg("idx"), nb::arg("side"), nb::arg("price"), nb::arg("qty"))
        .def("advance_time", [](Orchestrator& o, std::size_t idx,
                                std::uint64_t dt_ns) { o.advance_time(idx, dt_ns); })
        .def("cancel_strategy",
             [](Orchestrator& o, std::size_t idx, std::uint64_t ref) {
                 return o.cancel_strategy(idx, OrderId{ref});
             })
        .def("drain_all", [](Orchestrator& o) { o.drain_all(); })
        .def("advance_all", [](Orchestrator& o, std::uint64_t dt_ns) { o.advance_all(dt_ns); })
        .def(
            "sim",
            [](const Orchestrator& o, std::size_t idx) -> const ExecutionSimulator& {
                return o.sim(idx);
            },
            nb::arg("idx"), nb::rv_policy::reference_internal,
            "Per-instrument simulator; inspect with the ExecutionSimulator API.")
        .def(
            "global_digest",
            [](const Orchestrator& o) {
                const auto d = o.global_digest();
                char hex[65];
                static_cast<void>(Sha256::hex(d, hex));
                return std::string(hex, 64);
            },
            "SHA-256 fold over per-instrument digests in index order.")
        .def("audit", [](const Orchestrator& o) { return o.audit(); });

    // --- corpus generation -------------------------------------------------
    m.def(
        "sample_corpus",
        [](std::uint64_t seed, std::size_t count, std::int64_t price_center,
           std::int64_t price_span) -> nb::bytes {
            testing::CorpusSpec spec{};
            spec.seed = seed;
            spec.count = count;
            spec.price_center = price_center;
            spec.price_span = price_span;
            const auto raw = testing::encode_corpus(testing::make_corpus(spec));
            return nb::bytes(reinterpret_cast<const char*>(raw.data()),
                             static_cast<std::size_t>(raw.size()));
        },
        nb::arg("seed") = 0x5EEDULL, nb::arg("count") = 4096, nb::arg("price_center") = 0,
        nb::arg("price_span") = 10'000'000,
        "Deterministic synthetic ITCH capture (same generator as mog-gen-sample).");

    // --- build identity ----------------------------------------------------
    m.def("build_info", []() {
        nb::dict d;
        d["version"] = MOG_VERSION;
#if defined(MOG_PROFILE_PORTABLE)
        d["profile"] = "portable";
#elif defined(MOG_PROFILE_FRONTIER)
              d["profile"] = "frontier";
#else
              d["profile"] = "lab";
#endif
        d["compiler"] = std::string(kCompilerId) + " " + kCompilerVersion;
        d["cxx_standard"] = kCxxStandard;
        d["stdlib"] = std::string(kStdlibName);
        d["contracts_default"] =
            contracts::current_mode() == contracts::Mode::enforce
                ? "enforce"
                : (contracts::current_mode() == contracts::Mode::observe ? "observe" : "ignore");
        return d;
    });
}
