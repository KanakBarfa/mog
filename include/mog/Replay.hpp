// Replay orchestration: file loading (mmap with MAP_POPULATE, read fallback),
// raw-ITCH and MoldUDP64 ingestion through the session-tolerant parser, book
// application, and a deterministic summary. The `mog replay` CLI is a thin
// wrapper over run_replay(); tests drive the same entry point in-process.
#pragma once

#include <mog/ITCHParser.hpp>
#include <mog/MoldUdp.hpp>
#include <mog/OrderBook.hpp>
#include <mog/Session.hpp>
#include <mog/SessionFeed.hpp>

#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

namespace mog::replay {

enum class Format : std::uint8_t {
    itch_raw = 0,
    moldudp64 = 1,
    // NASDAQ BinaryFILE-style payload framing: 2-byte BE length + message.
    // This is how Nasdaq's public sample files are distributed.
    length_prefixed = 2,
};

enum class Error : std::uint8_t {
    io_error = 1,
    parse_error,
    mold_gap,
    empty_stream,
};

[[nodiscard]] constexpr std::string_view error_text(Error e) noexcept {
    switch (e) {
    case Error::io_error:
        return "io_error";
    case Error::parse_error:
        return "parse_error";
    case Error::mold_gap:
        return "mold_gap";
    case Error::empty_stream:
        return "empty_stream";
    }
    return "?";
}

// Whole-file mapping. MAP_POPULATE faults every page up front so replay timing
// excludes minor-fault noise; falls back to a plain read when mmap cannot (or
// should not) be used. Byte content is identical either way - digests must not
// depend on how the file arrived in memory.
class MappedFile {
public:
    MappedFile() noexcept = default;
    ~MappedFile() {
        if (map_)
            ::munmap(map_, size_);
        else
            delete[] bytes_;
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& o) noexcept
        : map_(std::exchange(o.map_, nullptr)), bytes_(std::exchange(o.bytes_, nullptr)),
          size_(std::exchange(o.size_, std::size_t{0})) {}
    MappedFile& operator=(MappedFile&&) = delete;

    [[nodiscard]] static std::expected<MappedFile, Error> load(const char* path, bool prefer_mmap) {
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return std::unexpected(Error::io_error);
        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            return std::unexpected(Error::io_error);
        }
        if (st.st_size == 0) {
            ::close(fd);
            return std::unexpected(Error::empty_stream);
        }
        const auto n = static_cast<std::size_t>(st.st_size);
        if (prefer_mmap) {
            void* p = ::mmap(nullptr, n, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
            if (p != MAP_FAILED) {
                ::close(fd);
                MappedFile f;
                f.map_ = static_cast<unsigned char*>(p);
                f.size_ = n;
                return f;
            } // mmap failed: fall through to read()
        }
        auto* buf = new (std::nothrow) unsigned char[n];
        if (buf == nullptr) {
            ::close(fd);
            return std::unexpected(Error::io_error);
        }
        std::size_t got = 0;
        while (got < n) {
            const ssize_t r = ::read(fd, buf + got, n - got);
            if (r <= 0) {
                ::close(fd);
                return std::unexpected(Error::io_error);
            }
            got += static_cast<std::size_t>(r);
        }
        ::close(fd);
        MappedFile f;
        f.bytes_ = buf;
        f.size_ = n;
        return f;
    }

    [[nodiscard]] const unsigned char* data() const noexcept {
        return map_ != nullptr ? map_ : bytes_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    unsigned char* map_ = nullptr;
    unsigned char* bytes_ = nullptr;
    std::size_t size_ = 0;
};

struct Options {
    Format format = Format::itch_raw;
    // Contract-max band (INT32_MAX ticks = ~$214k); a $1200 band once
    // silently dropped AMZN.
    OrderBook<>::Config book{1u << 22, {0, INT32_MAX, 24576}};
    bool use_mmap = true; // read() fallback still applied automatically

    // Arena from input size (est msgs x2), bounded by the default.
    [[nodiscard]] static Options sized_for(std::size_t bytes) noexcept {
        Options o;
        constexpr std::size_t kMinFrame = 12;
        // Full-universe sessions carry hundreds of millions of refs; peak
        // concurrent live orders passed 4M on the official full-day sample,
        // so the ceiling scales past the default instead of silently
        // rejecting adds (13.4M arena_full on itch50_05_15 before this).
        constexpr std::size_t kMaxArena = 1u << 24;
        const std::size_t est_msgs = bytes / kMinFrame + 1;
        std::size_t cap = 1u << 12;
        while (cap < est_msgs * 2 && cap < kMaxArena)
            cap <<= 1;
        o.book.arena_capacity = cap;
        // Full-universe days need tens of thousands of price neighborhoods.
        const std::size_t pool = est_msgs / 64;
        o.book.ladder.page_pool = pool < (1u << 8)                 ? (1u << 8)
                                  : pool > o.book.ladder.page_pool ? o.book.ladder.page_pool
                                                                   : pool;
        return o;
    }
};

struct Summary {
    std::size_t bytes_consumed = 0;
    std::size_t decoded = 0;
    std::size_t skipped = 0;
    std::size_t skipped_by_type[256] = {};
    std::size_t book_events_failed = 0;
    std::int64_t best_bid_ticks = 0; // authoritative read of the final book
    std::int64_t best_ask_ticks = 0;
    std::size_t live_orders = 0;
    std::uint64_t trace_digest = 0xcbf29ce484222325ULL; // FNV offset basis
    std::uint64_t first_ts_ns = 0;                      // 0 when nothing decoded
    std::uint64_t last_ts_ns = 0;
    // Session-level state (G3): populated on every run.
    session::Phase phase = session::Phase::idle;
    std::size_t halt_windows = 0;
    std::size_t noii_updates = 0;
    std::vector<session::CrossRecord> close_crosses;
};

struct Failure {
    Error code;
    ParseError parse{};     // meaningful when code == parse_error
    mold::ErrorInfo mold{}; // meaningful when code == mold_gap
};

namespace detail {

struct ReplaySink {
    OrderBook<>& book;
    Summary& out;

    void on_message(const Message& m) noexcept {
        const BookTick tick = book.apply(m, nullptr);
        if (!ok(tick))
            ++out.book_events_failed;
        out.trace_digest = trace_hash(m, out.trace_digest);
        // Every subset message leads with the same 11-byte header layout, but
        // the union only activates the matching member - read ts through it.
        std::uint64_t ts = 0;
        switch (m.type) {
        case 'A':
            ts = m.add_order.header.ts_ns.value;
            break;
        case 'F':
            ts = m.add_order_attribution.header.ts_ns.value;
            break;
        case 'E':
            ts = m.order_executed.header.ts_ns.value;
            break;
        case 'C':
            ts = m.order_executed_with_price.header.ts_ns.value;
            break;
        case 'X':
            ts = m.order_cancel.header.ts_ns.value;
            break;
        case 'D':
            ts = m.order_delete.header.ts_ns.value;
            break;
        case 'U':
            ts = m.order_replace.header.ts_ns.value;
            break;
        default:
            break;
        }
        if (out.first_ts_ns == 0 || ts < out.first_ts_ns)
            out.first_ts_ns = ts;
        if (ts > out.last_ts_ns)
            out.last_ts_ns = ts;
    }
};

} // namespace detail

// Streams [data, data+len) into a fresh OrderBook. Deterministic: identical
// input bytes produce an identical Summary regardless of format path or how
// the bytes were loaded.
[[nodiscard]] inline std::expected<Summary, Failure>
run_replay(std::span<const unsigned char> bytes, const Options& opts) {
    if (bytes.empty())
        return std::unexpected(Failure{Error::empty_stream});

    OrderBook<> book(opts.book);
    Summary summary;
    detail::ReplaySink sink{book, summary};
    const KernelTable& kernels = active_kernels();
    session::Model session_model;

    if (opts.format == Format::itch_raw) {
        struct Listener {
            session::Model& model;
            void operator()(char type, const unsigned char* p, std::size_t n) const noexcept {
                static_cast<void>(session::detail::feed_frame(model, type, p, n));
            }
        } listener{session_model};
        auto res =
            parse_itch_session_listen_as(bytes.data(), bytes.size(), sink, kernels, listener);
        if (!res)
            return std::unexpected(Failure{Error::parse_error, res.error()});
        summary.decoded = res->decoded;
        summary.skipped = res->skipped;
        for (std::size_t i = 0; i < 256; ++i)
            summary.skipped_by_type[i] = res->skipped_by_type[i];
        summary.bytes_consumed = bytes.size();
    } else if (opts.format == Format::length_prefixed) {
        struct Listener2 {
            session::Model& model;
            void operator()(char t, const unsigned char* p, std::size_t n) const noexcept {
                static_cast<void>(session::detail::feed_frame(model, t, p, n));
            }
        } listener{session_model};
        std::size_t off = 0;
        while (off + 2 <= bytes.size()) {
            const std::size_t mlen = static_cast<std::size_t>(wire::load_be16(bytes.data() + off));
            off += 2;
            if (mlen == 0 || bytes.size() - off < mlen) {
                // A zero length or short payload is a framing fault with the
                // byte offset preserved for diagnosis.
                return std::unexpected(
                    Failure{Error::parse_error, ParseError{DecodeError::truncated, off - 2}});
            }
            auto res =
                parse_itch_session_listen_as(bytes.data() + off, mlen, sink, kernels, listener);
            if (!res)
                return std::unexpected(Failure{Error::parse_error, res.error()});
            summary.decoded += res->decoded;
            summary.skipped += res->skipped;
            for (std::size_t i = 0; i < 256; ++i)
                summary.skipped_by_type[i] += res->skipped_by_type[i];
            off += mlen;
        }
        if (off != bytes.size())
            return std::unexpected(
                Failure{Error::parse_error, ParseError{DecodeError::truncated, off}});
        summary.bytes_consumed = off;
    } else {

        // session-tolerant decoder so framing and decoding errors stay distinct
        // and BOTH are loud.
        struct Chain {
            OrderBook<>& book;
            Summary& out;
            const KernelTable& kernels;
            session::Model& model_ref;
            bool failed = false;
            ParseError err{};
            void operator()(const unsigned char* p, std::size_t n, std::uint64_t) noexcept {
                if (failed)
                    return;
                detail::ReplaySink inner{book, out};
                struct L {
                    session::Model& model;
                    void operator()(char t2, const unsigned char* p2,
                                    std::size_t n2) const noexcept {
                        static_cast<void>(session::detail::feed_frame(model, t2, p2, n2));
                    }
                } l{model_ref};
                auto res = parse_itch_session_listen_as(p, n, inner, kernels, l);
                if (!res) {
                    failed = true;
                    err = res.error();
                    return;
                }
                out.decoded += res->decoded;
                out.skipped += res->skipped;
                for (std::size_t i = 0; i < 256; ++i)
                    out.skipped_by_type[i] += res->skipped_by_type[i];
            }
        } chain{book, summary, kernels, session_model};
        auto res = mold::parse_moldudp64(bytes.data(), bytes.size(), chain);
        if (!res)
            return std::unexpected(Failure{Error::mold_gap, ParseError{}, res.error()});
        if (chain.failed)
            return std::unexpected(Failure{Error::parse_error, chain.err});
        summary.bytes_consumed = *res;
    }

    summary.live_orders = book.live_orders();
    summary.best_bid_ticks = book.best_bid();
    summary.best_ask_ticks = book.best_ask();
    summary.phase = session_model.phase();
    summary.halt_windows = session_model.halt_windows().size();
    summary.noii_updates = session_model.noii_updates();
    summary.close_crosses = session_model.close_crosses();
    return summary;
}

} // namespace mog::replay
