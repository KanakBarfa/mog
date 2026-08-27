// Trade reconstruction from E/C messages (G4): E prices from live book
// state, C carries its own price; unknown-ref prints count, never drop.
// The canonical CSV is the diff currency vs official trade files.
#pragma once

#include <mog/OrderBook.hpp>
#include <mog/Replay.hpp>
#include <mog/Session.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace mog::trades {

struct Print {
    std::uint64_t match_number = 0;
    std::uint32_t locate = 0;
    std::uint64_t ts_ns = 0;
    std::int64_t price_ticks = 0; // C uses the wire price, E the level price
    std::uint32_t shares = 0;
    bool printable = false; // only meaningful for C; E prints are printable
    bool from_execute_with_price = false;
};

struct Summary {
    std::size_t bytes_consumed = 0;
    std::size_t decoded = 0;
    std::size_t skipped = 0;
    std::size_t prints = 0;              // E + C reconstructed
    std::size_t unpriced_executions = 0; // book rejected the ref; counted loudly
    std::size_t book_events_failed = 0;
    std::size_t prints_during_halts = 0; // suspect fills, visible per G3
    std::vector<Print> records;
};

namespace detail {

class Recorder {
public:
    explicit Recorder(const replay::Options& opts) : book_(opts.book) {}

    // Session-aware recording: standard frames must feed the model before
    // print classification means anything.
    void on_frame(char type, const unsigned char* p, std::size_t n) noexcept {
        static_cast<void>(mog::session::detail::feed_frame(model_, type, p, n));
    }

    [[nodiscard]] const session::Model& session() const noexcept { return model_; }

    void on_message(const Message& m) noexcept {
        const BookTick tick = book_.apply(m, nullptr);
        if (!ok(tick))
            ++summary_.book_events_failed;
        if (m.type == 'E') {
            record(m.order_executed.header, m.order_executed.match_number.value,
                   static_cast<std::uint64_t>(m.order_executed.executed_shares.units), tick,
                   /*wire_price=*/0, /*printable=*/true, /*with_price=*/false);
        } else if (m.type == 'C') {
            record(m.order_executed_with_price.header,
                   m.order_executed_with_price.match_number.value,
                   static_cast<std::uint64_t>(m.order_executed_with_price.executed_shares.units),
                   tick, m.order_executed_with_price.execution_price.ticks,
                   m.order_executed_with_price.printable, true);
        }
    }

    [[nodiscard]] const Summary& summary() const noexcept { return summary_; }

private:
    void record(const Header& hdr, std::uint64_t match, std::uint64_t shares, const BookTick& tick,
                std::int64_t wire_price, bool printable, bool with_price) noexcept {
        if (!ok(tick)) {
            // Unknown ref or over-execute: a print we cannot price from the
            // book. Count it - divergence hunting needs the misses visible.
            ++summary_.unpriced_executions;
            return;
        }
        if (model_.was_halted_at(hdr.locate.value, hdr.ts_ns.value))
            ++summary_.prints_during_halts;
        ++summary_.prints;
        summary_.records.push_back(Print{
            .match_number = match,
            .locate = hdr.locate.value,
            .ts_ns = hdr.ts_ns.value,
            .price_ticks = with_price ? wire_price : tick.price_ticks,
            .shares = static_cast<std::uint32_t>(shares),
            .printable = printable,
            .from_execute_with_price = with_price,
        });
    }

    OrderBook<> book_;
    session::Model model_;
    Summary summary_;
};

} // namespace detail

// Reconstructs prints over [data, data+len). Records keep stream order.
[[nodiscard]] inline std::expected<Summary, replay::Failure>
run_trades(std::span<const unsigned char> bytes, const mog::replay::Options& opts) {
    if (bytes.empty())
        return std::unexpected(replay::Failure{replay::Error::empty_stream});

    detail::Recorder recorder(opts);
    auto& sink = recorder;
    const KernelTable& kernels = active_kernels();
    SessionStats summary_stats;

    if (opts.format == replay::Format::length_prefixed) {
        struct Listener {
            detail::Recorder& rec;
            void operator()(char t, const unsigned char* p, std::size_t n) const noexcept {
                rec.on_frame(t, p, n);
            }
        } listener{recorder};
        std::size_t off = 0;
        while (off + 2 <= bytes.size()) {
            const std::size_t mlen =
                static_cast<std::size_t>(mog::wire::load_be16(bytes.data() + off));
            off += 2;
            if (mlen == 0 || bytes.size() - off < mlen)
                return std::unexpected(replay::Failure{
                    replay::Error::parse_error, ParseError{DecodeError::truncated, off - 2}});
            auto res = parse_itch_session_listen_as(bytes.data() + off, mlen, sink,
                                                    active_kernels(), listener);
            if (!res)
                return std::unexpected(replay::Failure{replay::Error::parse_error, res.error()});
            summary_stats.decoded += res->decoded;
            summary_stats.skipped += res->skipped;
            off += mlen;
        }
        if (off != bytes.size())
            return std::unexpected(replay::Failure{replay::Error::parse_error,
                                                   ParseError{DecodeError::truncated, off}});
        Summary out = recorder.summary();
        out.decoded = summary_stats.decoded;
        out.skipped = summary_stats.skipped;
        out.bytes_consumed = off;
        return out;
    }

    if (opts.format == replay::Format::itch_raw) {
        struct Listener {
            detail::Recorder& rec;
            void operator()(char t, const unsigned char* p, std::size_t n) const noexcept {
                rec.on_frame(t, p, n);
            }
        } listener{recorder};
        auto res =
            parse_itch_session_listen_as(bytes.data(), bytes.size(), sink, kernels, listener);
        if (!res)
            return std::unexpected(replay::Failure{replay::Error::parse_error, res.error()});
        Summary out = recorder.summary();
        out.decoded = res->decoded;
        out.skipped = res->skipped;
        out.bytes_consumed = bytes.size();
        return out;
    }

    struct Chain {
        detail::Recorder& recorder;
        const KernelTable& kernels_;
        std::size_t decoded = 0;
        std::size_t skipped = 0;
        bool failed = false;
        ParseError err{};
        void operator()(const unsigned char* p, std::size_t n, std::uint64_t) noexcept {
            if (failed)
                return;
            struct L {
                detail::Recorder& rec;
                void operator()(char t, const unsigned char* p, std::size_t n) const noexcept {
                    rec.on_frame(t, p, n);
                }
            } l{recorder};
            auto res = parse_itch_session_listen_as(p, n, recorder, kernels_, l);
            if (!res) {
                failed = true;
                err = res.error();
                return;
            }
            decoded += res->decoded;
            skipped += res->skipped;
        }
    } chain{recorder, active_kernels()};
    auto res = mold::parse_moldudp64(bytes.data(), bytes.size(), chain);
    if (!res)
        return std::unexpected(replay::Failure{replay::Error::mold_gap, ParseError{}, res.error()});
    if (chain.failed)
        return std::unexpected(replay::Failure{replay::Error::parse_error, chain.err});
    Summary out = recorder.summary();
    out.decoded = chain.decoded;
    out.skipped = chain.skipped;
    out.bytes_consumed = *res;
    return out;
}

[[nodiscard]] inline std::string csv_header() {
    return "match_number,locate,ts_ns,price_ticks,shares,printable,with_price";
}

[[nodiscard]] inline std::string to_csv(const Print& p) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%llu,%u,%llu,%lld,%u,%c,%c",
                  static_cast<unsigned long long>(p.match_number), p.locate,
                  static_cast<unsigned long long>(p.ts_ns), static_cast<long long>(p.price_ticks),
                  p.shares, p.printable ? 'Y' : 'N', p.from_execute_with_price ? 'Y' : 'N');
    return std::string(buf);
}

} // namespace mog::trades
