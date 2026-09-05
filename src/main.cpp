// CLI entry: info, selftest, gen-sample-driven replay over raw ITCH or
// MoldUDP64 captures.

#include <mog/AllocGuard.hpp>
#include <mog/Build.hpp>
#include <mog/Contracts.hpp>
#include <mog/Replay.hpp>
#include <mog/SimRun.hpp>
#include <mog/Tearsheet.hpp>
#include <mog/TradeDiff.hpp>
#include <mog/Trades.hpp>
#include <mog/Types.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <string_view>
#include <unistd.h>

namespace {

enum class CliError {
    unknown_command,
    missing_arg,
    bad_number,
    io_error,
    parse_error,
    mold_gap,
    empty_stream,
};

[[nodiscard]] std::string_view error_text(CliError e) noexcept {
    switch (e) {
    case CliError::unknown_command:
        return "unknown command";
    case CliError::missing_arg:
        return "missing argument";
    case CliError::bad_number:
        return "bad number";
    case CliError::io_error:
        return "io error";
    case CliError::parse_error:
        return "parse error";
    case CliError::mold_gap:
        return "moldudp64 sequence gap";
    case CliError::empty_stream:
        return "empty stream";
    }
    return "?";
}

void print_info() noexcept {
    std::printf("mog %s\n", MOG_VERSION_STRING);
    std::printf("profile: %s\n", ::mog::kBuildProfile);
    std::printf("compiler: %s %s\n", ::mog::kCompilerId, ::mog::kCompilerVersion);
    std::printf("stdlib: %s\n", ::mog::kStdlibName);
    std::printf("cxx_standard: %d\n", ::mog::kCxxStandard);
    std::printf("features:\n");
#define F(name) std::printf("  %-24s %s\n", #name, name ? "yes" : "no")
    F(MOG_HAS_EXPECTED);
    F(MOG_HAS_SATURATE_ARITHMETIC);
    F(MOG_HAS_STD_SIMD);
    F(MOG_HAS_INPLACE_VECTOR);
    F(MOG_HAS_FUNCTION_REF);
    F(MOG_HAS_ATOMIC_MIN_MAX);
    F(MOG_HAS_HIVE);
    F(MOG_HAS_PACK_INDEXING);
    F(MOG_HAS_PLACEHOLDER_VARIABLES);
    F(MOG_HAS_DELETED_WITH_REASON);
    F(MOG_HAS_EMBED);
    F(MOG_HAS_REFLECTION);
    F(MOG_HAS_EXPANSION_STATEMENTS);
    F(MOG_HAS_NATIVE_CONTRACTS);
#undef F
    std::printf("isa:");
#ifdef __x86_64__
    __builtin_cpu_init();
    if (__builtin_cpu_supports("sse4.2"))
        std::printf(" sse4.2");
    if (__builtin_cpu_supports("avx2"))
        std::printf(" avx2");
    if (__builtin_cpu_supports("avx512f"))
        std::printf(" avx512f");
#endif
    std::printf("\n");
}

[[nodiscard]] int run_selftest() noexcept {
    // Saturating public API must clamp; raw paths hold under bounds contracts.
    const auto max_price = mog::Price{INT64_MAX};
    const auto one = mog::Price{1};
    if (max_price.add(one).ticks != INT64_MAX)
        return 1;
    if (mog::Price{5}.raw_add(mog::Price{7}).ticks != 12)
        return 1;

    // A returning handler keeps the process alive while still reporting the trip.
    static bool tripped = false;
    const mog::contracts::Mode saved = mog::contracts::current_mode();
    mog::contracts::set_mode(mog::contracts::Mode::enforce);
    mog::contracts::set_handler(
        [](const char*, const char*, int, const char*) noexcept { tripped = true; });
    const mog::Qty bad = mog::Qty{0}.raw_sub_nonneg(mog::Qty{1});
    static_cast<void>(bad);
    mog::contracts::set_handler(nullptr);
    mog::contracts::set_mode(saved);
    if (!tripped)
        return 1;

    // Guarded region performs only stack arithmetic.
    mog::alloc::arm();
    std::int64_t acc = 0;
    for (std::int64_t i = 0; i < 1000; ++i)
        acc += i;
    mog::alloc::disarm();
    if (acc != 499500)
        return 1;
    return 0;
}

// Shared flag parsing for stream-consuming subcommands (replay, trades).
struct StreamArgs {
    const char* path = nullptr;
    const char* diff_ref = nullptr;
    mog::replay::Options opts;
    bool json = false;
    const char* csv_out = nullptr;
};

[[nodiscard]] std::expected<StreamArgs, CliError> parse_stream_args(int argc, char** argv) {
    StreamArgs a;
    for (int i = 0; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--mold")
            a.opts.format = mog::replay::Format::moldudp64;
        else if (arg == "--lenprefix")
            a.opts.format = mog::replay::Format::length_prefixed;
        else if (arg == "--pool") {
            if (++i >= argc)
                return std::unexpected(CliError::missing_arg);
            a.opts.book.ladder.page_pool =
                static_cast<std::size_t>(std::strtoul(argv[i], nullptr, 10));
        } else if (arg == "--no-mmap")
            a.opts.use_mmap = false;
        else if (arg == "--json")
            a.json = true;
        else if (arg == "--hi-tick") {
            if (++i >= argc)
                return std::unexpected(CliError::missing_arg);
            a.opts.book.ladder.hi_tick = std::strtoll(argv[i], nullptr, 10);
        } else if (arg == "--lo-tick") {
            if (++i >= argc)
                return std::unexpected(CliError::missing_arg);
            a.opts.book.ladder.lo_tick = std::strtoll(argv[i], nullptr, 10);
        } else if (arg == "--csv") {
            if (++i >= argc)
                return std::unexpected(CliError::missing_arg);
            a.csv_out = argv[i];
        } else if (arg == "--diff") {
            if (++i >= argc)
                return std::unexpected(CliError::missing_arg);
            a.diff_ref = argv[i];
        } else if (!arg.empty() && arg[0] == '-') {
            return std::unexpected(CliError::unknown_command);
        } else {
            a.path = argv[i];
        }
    }
    if (a.path == nullptr)
        return std::unexpected(CliError::missing_arg);
    return a;
}

[[nodiscard]] CliError to_cli(mog::replay::Error e) noexcept {
    switch (e) {
    case mog::replay::Error::io_error:
        return CliError::io_error;
    case mog::replay::Error::parse_error:
        return CliError::parse_error;
    case mog::replay::Error::mold_gap:
        return CliError::mold_gap;
    case mog::replay::Error::empty_stream:
        break;
    }
    return CliError::empty_stream;
}

// --- simrun: scripted ExecutionSimulator runs with full config surface ---

[[nodiscard]] std::expected<int, CliError> run_simrun_command(int argc, char** argv) noexcept {
    const char* path = nullptr;
    const char* events_out = nullptr;
    mog::SimConfig cfg; // runnable defaults; flags below override
    for (int i = 0; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto need = [&]() -> const char* { return i + 1 < argc ? argv[i + 1] : nullptr; };
        if (a == "--arena" && need()) {
            cfg.book.arena_capacity = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--lo-tick" && need()) {
            cfg.book.ladder.lo_tick = std::strtoll(argv[++i], nullptr, 10);
        } else if (a == "--hi-tick" && need()) {
            cfg.book.ladder.hi_tick = std::strtoll(argv[++i], nullptr, 10);
        } else if (a == "--seed" && need()) {
            cfg.seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--depletion" && need()) {
            // bid,ask in units per microsecond
            double b = 0, q = 0;
            const int got = std::sscanf(argv[++i], "%lf,%lf", &b, &q);
            if (got < 1)
                return std::unexpected(CliError::bad_number);
            cfg.bid_depletion_per_us = b;
            cfg.ask_depletion_per_us = q; // omitted ask defaults to 0
        } else if (a == "--hawkes" && need()) {
            double k = 0;
            unsigned long long tau = 0;
            if (std::sscanf(argv[++i], "%lf,%llu", &k, &tau) != 2)
                return std::unexpected(CliError::bad_number);
            cfg.hawkes_kappa = k;
            cfg.hawkes_decay_ns = tau;
        } else if (a == "--latency-ns" && need()) {
            const std::uint32_t v =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            cfg.parse_latency_ns = v;
            cfg.decision_latency_ns = v;
            cfg.wire_latency_ns = v;
        } else if (a == "--jitter" && need()) {
            // uniform:MAX | none
            std::string_view j = argv[++i];
            if (j.rfind("uniform:", 0) == 0) {
                cfg.jitter_kind = mog::JitterKind::uniform;
                cfg.jitter_max_ns =
                    static_cast<std::uint32_t>(std::strtoul(j.substr(8).data(), nullptr, 10));
            } else if (j == "none") {
                cfg.jitter_kind = mog::JitterKind::none;
            } else {
                return std::unexpected(CliError::bad_number);
            }
        } else if (a == "--maker-fee-bps" && need()) {
            cfg.maker_fee_bps = std::strtoll(argv[++i], nullptr, 10);
        } else if (a == "--taker-fee-bps" && need()) {
            cfg.taker_fee_bps = std::strtoll(argv[++i], nullptr, 10);
        } else if (a == "--digest-mode" && need()) {
            const std::string_view m = argv[++i];
            if (m == "fast")
                cfg.digest_mode = mog::DigestMode::fast;
            else if (m == "golden")
                cfg.digest_mode = mog::DigestMode::golden;
            else
                return std::unexpected(CliError::bad_number);
        } else if (a == "--events-csv" && need()) {
            events_out = argv[++i];
        } else if (a == "--json") {
            // Output is a single JSON line regardless; accepted for
            // symmetry with replay/trades.
        } else if (!a.empty() && a[0] == '-') {
            return std::unexpected(CliError::unknown_command);
        } else {
            path = argv[i];
        }
    }
    if (path == nullptr)
        return std::unexpected(CliError::missing_arg);

    auto file = mog::replay::MappedFile::load(path, true);
    if (!file)
        return std::unexpected(to_cli(file.error()));
    std::string text{reinterpret_cast<const char*>(file->data()), file->size()};
    auto result = mog::simrun::run(text, cfg);
    if (!result) {
        std::fprintf(stderr, "mog: bad script csv\n");
        return std::unexpected(CliError::parse_error);
    }
    const mog::simrun::Summary& r = *result;

    if (events_out != nullptr) {
        FILE* out = std::fopen(events_out, "w");
        if (out == nullptr)
            return std::unexpected(CliError::io_error);
        std::fwrite(mog::simrun::events_header().data(), 1, mog::simrun::events_header().size(),
                    out);
        std::fputc('\n', out);
        for (const auto& e : r.events) {
            const std::string line = mog::simrun::to_csv(e);
            std::fwrite(line.data(), 1, line.size(), out);
            std::fputc('\n', out);
        }
        std::fclose(out);
    }
    std::printf("{\"script_rows\":%zu,\"fills\":%zu,\"prints\":%zu,"
                "\"volume_ticks\":%lld,\"fees_cash\":%lld,\"digest_high\":"
                "\"0x%016llx\",\"digest_mode\":\"%s\",\"seed\":%llu}\n",
                r.script_rows, r.fills, r.prints, static_cast<long long>(r.volume_ticks),
                static_cast<long long>(r.fees_paid_cash),
                static_cast<unsigned long long>(r.digest_high),
                r.digest_mode == mog::DigestMode::fast ? "fast" : "golden",
                static_cast<unsigned long long>(cfg.seed));
    return 0;
}

[[nodiscard]] std::expected<int, CliError> run_tearsheet_command(int argc, char** argv) noexcept {
    const char* path = nullptr;
    bool json = false;
    for (int i = 0; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--json")
            json = true;
        else if (!a.empty() && a[0] == '-')
            return std::unexpected(CliError::unknown_command);
        else
            path = argv[i];
    }
    if (path == nullptr)
        return std::unexpected(CliError::missing_arg);
    auto file = mog::replay::MappedFile::load(path, true);
    if (!file)
        return std::unexpected(to_cli(file.error()));
    std::string text{reinterpret_cast<const char*>(file->data()), file->size()};
    mog::tearsheet::Report rep;
    if (!mog::tearsheet::compute_from_csv(text, rep)) {
        std::fprintf(stderr, "mog: not a simrun events csv\n");
        return std::unexpected(CliError::parse_error);
    }
    if (json) {
        std::printf(
            "{\"fills\":%zu,\"prints\":%zu,\"volume_ticks\":%lld,"
            "\"fees_cash\":%lld,\"fees_maker\":%lld,\"fees_taker\":%lld,"
            "\"inventory\":%lld,\"cash_pnl\":%lld,\"equity_pnl\":%lld,"
            "\"max_dd_bps\":%.2f,\"hit_rate\":%.4f}\n",
            rep.fills, rep.prints, static_cast<long long>(rep.volume_ticks),
            static_cast<long long>(rep.fees_cash), static_cast<long long>(rep.fees_maker_cash),
            static_cast<long long>(rep.fees_taker_cash),
            static_cast<long long>(rep.final_inventory), static_cast<long long>(rep.cash_pnl_ticks),
            static_cast<long long>(rep.equity_pnl_ticks), rep.max_drawdown_bps, rep.hit_rate);
        return 0;
    }
    std::printf("tearsheet       %s\n", path);
    std::printf("activity        %zu fills, %zu prints\n", rep.fills, rep.prints);
    std::printf("volume          %lld ticks (%llu bought, %llu sold)\n",
                static_cast<long long>(rep.volume_ticks),
                static_cast<unsigned long long>(rep.buy_qty),
                static_cast<unsigned long long>(rep.sell_qty));
    std::printf("fees            %lld total (maker %lld / taker %lld)\n",
                static_cast<long long>(rep.fees_cash), static_cast<long long>(rep.fees_maker_cash),
                static_cast<long long>(rep.fees_taker_cash));
    std::printf(
        "pnl             cash=%lld equity=%lld (inv=%lld @ mid %lld)\n",
        static_cast<long long>(rep.cash_pnl_ticks), static_cast<long long>(rep.equity_pnl_ticks),
        static_cast<long long>(rep.final_inventory), static_cast<long long>(rep.final_mid_ticks));
    std::printf("risk            max_dd=%.2f bps, hit_rate=%.1f%%\n", rep.max_drawdown_bps,
                rep.hit_rate * 100.0);
    for (const auto& m : rep.markouts)
        std::printf("markout +%lluns   %.2f bps over %zu fills\n",
                    static_cast<unsigned long long>(m.horizon_ns), m.mean_bps, m.measured);
    return 0;
}

// Shared across stream-consuming subcommands: maps a library failure to the
// right exit path, printing precise diagnostics for the loud-error cases.
[[nodiscard]] std::expected<int, CliError>
report_stream_failure(const mog::replay::Failure& f) noexcept {
    switch (f.code) {
    case mog::replay::Error::parse_error:
        std::fprintf(stderr, "mog: parse error at byte %zu (%s)\n", f.parse.offset,
                     mog::error_text(f.parse.code).data());
        return std::unexpected(CliError::parse_error);
    case mog::replay::Error::mold_gap:
        std::fprintf(stderr,
                     "mog: moldudp64 %s at byte %zu: session '%.10s' "
                     "expected seq %llu got %llu\n",
                     mog::mold::error_text(f.mold.code).data(), f.mold.offset, f.mold.gap.session,
                     static_cast<unsigned long long>(f.mold.gap.expected_seq),
                     static_cast<unsigned long long>(f.mold.gap.got_seq));
        return std::unexpected(CliError::mold_gap);
    default:
        return std::unexpected(to_cli(f.code));
    }
}

[[nodiscard]] std::expected<std::pair<mog::replay::MappedFile, StreamArgs>, CliError>
load_stream(int argc, char** argv) {
    auto args = parse_stream_args(argc, argv);
    if (!args)
        return std::unexpected(args.error());
    auto file = mog::replay::MappedFile::load(args->path, args->opts.use_mmap);
    if (!file)
        return std::unexpected(to_cli(file.error()));
    // Size the arena and ladder pool for the input without disturbing user-specified options
    // (format flags etc. live in the same struct).
    const auto sized = mog::replay::Options::sized_for(file->size());
    args->opts.book.arena_capacity = sized.book.arena_capacity;
    if (args->opts.book.ladder.page_pool < sized.book.ladder.page_pool)
        args->opts.book.ladder.page_pool = sized.book.ladder.page_pool;
    return std::make_pair(std::move(*file), *args);
}

[[nodiscard]] std::expected<int, CliError> run_replay_command(int argc, char** argv) noexcept {
    auto loaded = load_stream(argc, argv);
    if (!loaded)
        return std::unexpected(loaded.error());
    auto [file, args] = std::move(loaded.value());
    const mog::replay::Options& opts = args.opts;
    const bool json = args.json;

    const auto t0 = std::chrono::steady_clock::now();
    auto result =
        mog::replay::run_replay(std::span<const unsigned char>{file.data(), file.size()}, opts);
    const auto t1 = std::chrono::steady_clock::now();
    if (!result)
        return report_stream_failure(result.error());

    const mog::replay::Summary& s = *result;
    // kNoTick sentinels mean "no liquidity"; report zeros instead of leaking
    // internal sentinels into user-facing output.
    const long long bid =
        s.best_bid_ticks == INT64_MAX ? 0 : static_cast<long long>(s.best_bid_ticks);
    const long long ask =
        s.best_ask_ticks == INT64_MAX ? 0 : static_cast<long long>(s.best_ask_ticks);
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double rate =
        s.decoded + s.skipped > 0 ? static_cast<double>(s.decoded + s.skipped) / (ms / 1e3) : 0.0;
    if (json) {
        std::printf("{\"bytes\":%zu,\"decoded\":%zu,\"skipped\":%zu,"
                    "\"book_events_failed\":%zu,\"live_orders\":%zu,"
                    "\"best_bid\":%lld,\"best_ask\":%lld,"
                    "\"first_ts\":%llu,\"last_ts\":%llu,"
                    "\"halt_windows\":%zu,\"noii_updates\":%zu,"
                    "\"close_crosses\":%zu,"
                    "\"digest\":\"0x%016llx\",\"ms\":%.3f,\"msg_per_s\":%.0f}\n",
                    s.bytes_consumed, s.decoded, s.skipped, s.book_events_failed, s.live_orders,
                    bid, ask, static_cast<unsigned long long>(s.first_ts_ns),
                    static_cast<unsigned long long>(s.last_ts_ns), s.halt_windows, s.noii_updates,
                    s.close_crosses.size(), static_cast<unsigned long long>(s.trace_digest), ms,
                    rate);
        return 0;
    }
    std::printf("file            %s (%zu bytes)\n", args.path, s.bytes_consumed);
    std::printf("messages        %zu decoded, %zu skipped\n", s.decoded, s.skipped);
    for (int c = 0; c < 256; ++c)
        if (s.skipped_by_type[c] > 0)
            std::printf("  skip '%c'       %zu\n", c, s.skipped_by_type[c]);
    std::printf("book events     %zu failed\n", s.book_events_failed);
    std::printf("final book      live=%zu best_bid=%lld best_ask=%lld\n", s.live_orders, bid, ask);
    std::printf("session         halts=%zu noii=%zu crosses=%zu\n", s.halt_windows, s.noii_updates,
                s.close_crosses.size());
    std::printf("timestamps       %llu .. %llu ns\n",
                static_cast<unsigned long long>(s.first_ts_ns),
                static_cast<unsigned long long>(s.last_ts_ns));
    std::printf("digest          0x%016llx\n", static_cast<unsigned long long>(s.trace_digest));
    std::printf("elapsed         %.1f ms (%.1f Mmsg/s)\n", ms, rate / 1e6);
    return 0;
}

[[nodiscard]] std::expected<int, CliError> run_trades_command(int argc, char** argv) noexcept {
    auto loaded = load_stream(argc, argv);
    if (!loaded)
        return std::unexpected(loaded.error());
    auto [file, args] = std::move(loaded.value());

    auto result = mog::trades::run_trades(std::span<const unsigned char>{file.data(), file.size()},
                                          args.opts);
    if (!result)
        return report_stream_failure(result.error());

    const mog::trades::Summary& t = *result;
    if (args.csv_out != nullptr) {
        FILE* out = std::fopen(args.csv_out, "w");
        if (out == nullptr)
            return std::unexpected(CliError::io_error);
        std::fprintf(out, "%s\n", mog::trades::csv_header().c_str());
        for (const mog::trades::Print& p : t.records) {
            const std::string line = mog::trades::to_csv(p);
            std::fwrite(line.data(), 1, line.size(), out);
            std::fputc('\n', out);
        }
        std::fclose(out);
    }
    if (args.json) {
        std::printf("{\"bytes\":%zu,\"prints\":%zu,\"unpriced\":%zu,"
                    "\"halted_prints\":%zu,\"decoded\":%zu,\"skipped\":%zu}\n",
                    t.bytes_consumed, t.prints, t.unpriced_executions, t.prints_during_halts,
                    t.decoded, t.skipped);
        return 0;
    }
    std::printf("file              %s (%zu bytes)\n", args.path, t.bytes_consumed);
    std::printf("prints            %zu reconstructed (%zu with-price C prints)\n", t.prints, [&] {
        std::size_t n = 0;
        for (const auto& p : t.records)
            n += p.from_execute_with_price;
        return n;
    }());
    std::printf("unpriced          %zu executions against unknown refs\n", t.unpriced_executions);
    std::printf("halted prints     %zu inside session halt windows\n", t.prints_during_halts);
    std::printf("messages          %zu decoded, %zu skipped\n", t.decoded, t.skipped);
    if (args.csv_out != nullptr)
        std::printf("csv               %s\n", args.csv_out);

    if (args.diff_ref != nullptr) {
        if (args.csv_out != nullptr) {
            FILE* ours = std::fopen(args.csv_out, "w");
            if (ours == nullptr)
                return std::unexpected(CliError::io_error);
            std::fprintf(ours, "%s\n", mog::trades::csv_header().c_str());
            for (const mog::trades::Print& p : t.records) {
                const std::string line = mog::trades::to_csv(p);
                std::fwrite(line.data(), 1, line.size(), ours);
                std::fputc('\n', ours);
            }
            std::fclose(ours);
        }
        auto slurp = [](const char* path) -> std::string {
            FILE* f = std::fopen(path, "rb");
            if (f == nullptr)
                return {};
            std::string out;
            char buf[4096];
            while (!std::feof(f) && !std::ferror(f)) {
                out.append(buf, std::fread(buf, 1, sizeof(buf), f));
            }
            std::fclose(f);
            return out;
        };
        const std::string ref_text = slurp(args.diff_ref);
        if (ref_text.empty())
            return std::unexpected(CliError::io_error);
        mog::trades::ParsedCsv po, pr;
        po.rows.reserve(t.records.size());
        for (const mog::trades::Print& p : t.records)
            po.rows[p.match_number] = {p.price_ticks, static_cast<std::uint32_t>(p.shares)};
        if (!mog::trades::parse_canonical_csv(ref_text, pr)) {
            std::fprintf(stderr, "mog: reference csv is not in canonical format\n");
            return std::unexpected(CliError::parse_error);
        }
        const mog::trades::DiffReport d = mog::trades::diff(po, pr);
        std::printf("diff vs          %s\n", args.diff_ref);
        std::printf("  rows           %zu ours, %zu reference\n", d.rows_ours, d.rows_ref);
        std::printf("  matched        %zu\n", d.matched);
        std::printf("  price mismatch %zu\n", d.price_mismatch);
        std::printf("  shares mismatch %zu\n", d.shares_mismatch);
        if (d.has_divergence)
            std::printf("  first divergence: match %llu ours(%lld,%u) "
                        "ref(%lld,%u)\n",
                        static_cast<unsigned long long>(d.first_divergence.match_number),
                        static_cast<long long>(d.first_divergence.ours_price),
                        d.first_divergence.ours_shares,
                        static_cast<long long>(d.first_divergence.ref_price),
                        d.first_divergence.ref_shares);
        else if (!d.clean() && d.rows_ours != d.rows_ref)
            std::printf("  coverage gap   %zu prints unmatched on one side\n",
                        d.rows_ours > d.rows_ref ? d.rows_ours - d.rows_ref
                                                 : d.rows_ref - d.rows_ours);
    }
    return 0;
}

void print_usage() noexcept {
    std::printf("usage: mog <command>\n"
                "  info              build and feature report (byte-stable across runs)\n"
                "  selftest          in-process invariant checks\n"
                "  replay <file>     L3 capture replay\n"
                "  trades <file>     reconstruct trade prints from E/C messages\n"
                "  simrun <script>   drive the execution simulator over a script CSV\n"
                "    --seed N --depletion B,A --hawkes K,TAU --latency-ns X\n"
                "    --jitter uniform:N|none --maker-fee-bps F --taker-fee-bps F\n"
                "    --events-csv PATH  per-fill/print log for tearsheet recompute\n"
                "    --mold          input is MoldUDP64-framed (gap detection on)\n"
                "    --no-mmap       read() instead of mmap(MAP_POPULATE)\n"
                "    --json          machine-readable summary line\n"
                "    --csv PATH      (trades) write canonical print records\n");
}

std::expected<int, CliError> dispatch(int argc, char** argv) noexcept {
    if (argc < 2) {
        print_usage();
        return 0;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "info") {
        print_info();
        return 0;
    }
    if (cmd == "selftest")
        return run_selftest();
    if (cmd == "replay") {
        if (argc < 3)
            return std::unexpected(CliError::missing_arg);
        return run_replay_command(argc - 2, argv + 2);
    }
    if (cmd == "simrun") {
        if (argc < 3)
            return std::unexpected(CliError::missing_arg);
        return run_simrun_command(argc - 2, argv + 2);
    }
    if (cmd == "tearsheet") {
        if (argc < 3)
            return std::unexpected(CliError::missing_arg);
        return run_tearsheet_command(argc - 2, argv + 2);
    }
    if (cmd == "trades") {
        if (argc < 3)
            return std::unexpected(CliError::missing_arg);
        return run_trades_command(argc - 2, argv + 2);
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }
    return std::unexpected(CliError::unknown_command);
}

} // namespace

int main(int argc, char** argv) {
    auto result = dispatch(argc, argv);
    if (result)
        return *result;
    const std::string_view text = error_text(result.error());
    std::fprintf(stderr, "mog: %.*s\n", static_cast<int>(text.size()), text.data());
    switch (result.error()) {
    case CliError::unknown_command:
    case CliError::missing_arg:
        return 2;
    case CliError::bad_number:
        return 3;
    case CliError::io_error:
        return 4;
    case CliError::parse_error:
        return 5;
    case CliError::mold_gap:
        return 6;
    case CliError::empty_stream:
        return 7;
    }
    return 2;
}
