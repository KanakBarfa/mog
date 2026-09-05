// Classifies unpriced executions (never-added vs added-then-gone) and emits
// per-locate print stats. Counts errors on BOTH sides of the stream:
// rejected adds never appear among execution misses.
//
// Usage: unpriced_classify <capture.itch> [stats.csv]
#include <mog/ITCHParser.hpp>
#include <mog/OrderBook.hpp>
#include <mog/Replay.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace mog;

namespace {

struct LocStats {
    std::uint64_t prints = 0;
    std::uint64_t unpriced = 0;
    std::int64_t min_tick = INT64_MAX;
    std::int64_t max_tick = INT64_MIN;
    std::uint64_t shares = 0;
    // notional in tick*share units; divide by shares later if needed
    unsigned __int128 notional_ts = 0;
};

struct Classifier {
    OrderBook<> book;
    // Light mode (argv[4] = "light"): full-day captures carry hundreds of
    // millions of order refs; node-based ref-fate maps then cost tens of GB
    // for diagnostics the headline numbers never read. Errors bucket as
    // unseen instead.
    bool light = false;
    std::unordered_set<std::uint64_t> seen;
    std::unordered_map<std::uint16_t, LocStats> by_locate;
    std::unordered_map<std::uint16_t, std::string> symbol;
    // key: error*2 + seen_flag
    std::array<std::uint64_t, 32> hist{};
    std::array<std::uint64_t, 256> by_type{};
    // Ref fate for stale-execution sub-classification:
    // 1=resting 2=deleted/canceled 3=fully drained 4=replaced away
    std::unordered_map<std::uint64_t, std::uint8_t> fate;
    std::array<std::uint64_t, 8> stale_by_fate{};
    std::array<std::uint64_t, 16> add_err{};
    struct PrintRow {
        std::uint16_t locate;
        std::uint64_t ts_ns;
        std::int64_t price_ticks;
        std::uint32_t shares;
    };
    std::vector<PrintRow> print_rows;

    void on_message(const Message& m) {
        const char t = m.type;
        ++by_type[static_cast<unsigned char>(t)];
        // Book maintenance first - stale state poisons classification.
        const BookTick mtick = book.apply(m, nullptr);
        // Add-side failures matter: a rejected A makes every later E against
        // that ref look stale even though it never rested here.
        if ((t == 'A' || t == 'F') && !ok(mtick))
            ++add_err[std::min<std::size_t>(mtick.error, add_err.size() - 1)];
        if (t == 'A' || t == 'F') {
            const Header& h = t == 'A' ? m.add_order.header : m.add_order_attribution.header;
            const std::uint64_t r =
                t == 'A' ? m.add_order.order_ref.value : m.add_order_attribution.order_ref.value;
            if (!light) {
                seen.insert(r);
                fate[r] = 1;
            }
            const char* sym =
                t == 'A' ? m.add_order.stock.data() : m.add_order_attribution.stock.data();
            std::string s8(sym, sym + 8);
            while (!s8.empty() && (s8.back() == ' ' || s8.back() == '\0'))
                s8.pop_back();
            symbol.emplace(h.locate.value, std::move(s8));
        }
        if (t == 'D') {
            if (!light)
                if (std::uint8_t& f = fate[m.order_delete.order_ref.value])
                    f = 2;
        } else if (t == 'X' && !light) {
            // A full cancel removes the order; a partial one leaves it
            // resting. The book is the authority - ask it.
            const std::uint64_t xr = m.order_cancel.order_ref.value;
            if (book.remaining_of(OrderId{xr}) == 0)
                if (std::uint8_t& f = fate[xr])
                    f = 2;
        } else if (t == 'U' && !light) {
            if (std::uint8_t& f = fate[m.order_replace.original_order_ref.value])
                f = 4;
            fate[m.order_replace.new_order_ref.value] = 1;
        }
        // E/C need no fate update: a stale execution is classified by the
        // D/U/X state the ref was last left in (drained refs still count as
        // seen=1, which is the honest bucket - the order existed).
        if (t != 'E' && t != 'C')
            return;
        const BookTick& tick = mtick;
        const std::uint16_t locate =
            (t == 'E' ? m.order_executed.header.locate : m.order_executed_with_price.header.locate)
                .value;
        auto& st = by_locate[locate];
        if (!ok(tick)) {
            const bool was_seen = t == 'E'
                                      ? seen.count(m.order_executed.order_ref.value) > 0
                                      : seen.count(m.order_executed_with_price.order_ref.value) > 0;
            ++hist[std::min<std::size_t>(tick.error, 15) * 2 + was_seen];
            if (was_seen) {
                const std::uint64_t r = t == 'E' ? m.order_executed.order_ref.value
                                                 : m.order_executed_with_price.order_ref.value;
                auto it = fate.find(r);
                ++stale_by_fate[it != fate.end() ? it->second : 0];
            }
            ++st.unpriced;
            return;
        }
        const std::int64_t px =
            t == 'C' ? m.order_executed_with_price.execution_price.ticks : tick.price_ticks;
        const std::uint32_t q =
            t == 'E'
                ? static_cast<std::uint32_t>(m.order_executed.executed_shares.units)
                : static_cast<std::uint32_t>(m.order_executed_with_price.executed_shares.units);
        ++st.prints;
        st.shares += q;
        print_rows.push_back({locate,
                              (t == 'E' ? m.order_executed.header.ts_ns.value
                                        : m.order_executed_with_price.header.ts_ns.value),
                              px, q});
        st.notional_ts += static_cast<unsigned __int128>(px) * q;
        st.min_tick = std::min(st.min_tick, px);
        st.max_tick = std::max(st.max_tick, px);
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <capture.itch> [stats.csv [prints.csv [light]]]\n",
                     argv[0]);
        return 2;
    }
    // Lazy private mmap: MAP_POPULATE (Replay.hpp) is a timing feature for
    // prefix replays and OOMs on captures larger than RAM. Full days need
    // evictable page cache, not prefaulted residency.
    const int fd = ::open(argv[1], O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "open failed\n");
        return 2;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        std::fprintf(stderr, "stat failed\n");
        return 2;
    }
    const std::size_t len = static_cast<std::size_t>(st.st_size);
    const unsigned char* base =
        static_cast<const unsigned char*>(::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0));
    ::close(fd);
    if (base == MAP_FAILED) {
        std::fprintf(stderr, "mmap failed\n");
        return 2;
    }
    Classifier c{
        OrderBook<>{replay::Options::sized_for(len).book}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
    if (argc > 3 + 1 && std::string_view(argv[4]) == "light")
        c.light = true;
    else
        c.seen.reserve(1u << 26);
    // BinaryFILE framing: 2-byte BE length + message. Only the core book
    // subset is decoded; everything else is skipped by its wire length.
    auto kernel = active_kernels();
    Message msg;
    std::size_t off = 0;
    while (off + 3 <= len) {
        const std::size_t n = wire::load_be16(base + off);
        const char type = static_cast<char>(base[off + 2]);
        off += 2;
        if (n == 0 || off + n > len)
            break;
        // Core book subset only; session-only types (S/R/H/Y/I/Q/P/...) are
        // skipped whole - re-decoding them would leave a stale Message.
        if (type == 'A' || type == 'F' || type == 'E' || type == 'C' || type == 'X' ||
            type == 'D' || type == 'U') {
            if (kernel.parse_one(base + off, n, msg))
                c.on_message(msg);
        }
        off += n;
        if ((off & 0xFFFFFFFull) < n)
            std::fprintf(stderr, "progress %.1f GB\n", off / 1073741824.0);
    }
    std::fprintf(stderr, "frames=%zu locates=%zu\n", off, c.by_locate.size());
    static const char* fnames[] = {"unknown", "resting?", "deleted", "drained", "replaced_away"};
    for (int f = 0; f < 5; ++f)
        if (c.stale_by_fate[f])
            std::fprintf(stderr, "stale fate=%s -> %llu\n", fnames[f],
                         static_cast<unsigned long long>(c.stale_by_fate[f]));
    static const char* enames[] = {
        "",           "unknown_order", "duplicate_ref", "over_execute", "bad_qty", "out_of_band",
        "arena_full", "empty_level",   "page_exhausted"};
    for (std::size_t e2 = 1; e2 < 9; ++e2)
        if (c.add_err[e2])
            std::fprintf(stderr, "ADD-FAIL %-14s -> %llu\n", enames[e2],
                         static_cast<unsigned long long>(c.add_err[e2]));
    std::fprintf(stderr, "live=%zu\n", c.book.live_orders());
    for (const char t : {'A', 'F', 'E', 'C', 'X', 'D', 'U'})
        std::fprintf(stderr, "%c=%llu\n", t,
                     static_cast<unsigned long long>(c.by_type[static_cast<unsigned char>(t)]));
    static const char* names[] = {
        "",           "unknown_order", "duplicate_ref", "over_execute", "bad_qty", "out_of_band",
        "arena_full", "empty_level",   "page_exhausted"};
    for (std::size_t e = 1; e < 9; ++e)
        for (int s = 0; s < 2; ++s)
            if (c.hist[e * 2 + s])
                std::fprintf(stderr, "%-14s seen=%d -> %llu\n", names[e], s,
                             static_cast<unsigned long long>(c.hist[e * 2 + s]));
    if (argc > 2) {
        std::ofstream out(argv[2]);
        out << "locate,symbol,prints,unpriced,min_tick,max_tick,shares,"
               "vwap_micro\n";
        for (auto& [loc, st] : c.by_locate) {
            if (st.prints == 0)
                continue;
            const long long vwap_um = static_cast<long long>(
                (static_cast<unsigned __int128>(st.notional_ts) * 1000000ull) / st.shares);
            out << loc << ',' << c.symbol[loc] << ',' << st.prints << ',' << st.unpriced << ','
                << st.min_tick << ',' << st.max_tick << ',' << st.shares << ',' << vwap_um << '\n';
        }
    }
    if (argc > 3) {
        std::ofstream pf(argv[3]);
        pf << "locate,symbol,ts_ns,price_ticks,shares\n";
        for (const auto& pr : c.print_rows)
            pf << pr.locate << ',' << c.symbol[pr.locate] << ',' << pr.ts_ns << ','
               << pr.price_ticks << ',' << pr.shares << '\n';
    }
}
