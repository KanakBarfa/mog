// Trade reconstruction (G4): hand-built streams with hand-computed prints.
// Every expected CSV row below was derived by hand from the constructed
// stream - this is the ground-truth harness checking itself before it
// checks anyone else.

#include <mog/TradeDiff.hpp>
#include <mog/Trades.hpp>

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

using mog::Message;

std::size_t g_ts = 1'000'000;

void append_be16(std::vector<unsigned char>& v, std::uint16_t x) {
    unsigned char b[2];
    mog::wire::store_be16(b, x);
    v.insert(v.end(), b, b + 2);
}

void append_be32(std::vector<unsigned char>& v, std::uint32_t x) {
    unsigned char b[4];
    mog::wire::store_be32(b, x);
    v.insert(v.end(), b, b + 4);
}

void append_be48(std::vector<unsigned char>& v, std::uint64_t x) {
    unsigned char b[6];
    mog::wire::store_be48(b, x);
    v.insert(v.end(), b, b + 6);
}

void append_be64(std::vector<unsigned char>& v, std::uint64_t x) {
    unsigned char b[8];
    mog::wire::store_be64(b, x);
    v.insert(v.end(), b, b + 8);
}

void append_header(std::vector<unsigned char>& m, char type, std::uint32_t locate) {
    m.push_back(static_cast<unsigned char>(type));
    append_be16(m, static_cast<std::uint16_t>(locate));
    append_be16(m, 7); // tracking
    append_be48(m, g_ts++);
}

// Add Order: ref, side ('B'/'S'), shares, 8-char symbol, price.
std::vector<unsigned char> add_order(char side, std::uint64_t ref, std::uint32_t shares,
                                     std::uint32_t price) {
    std::vector<unsigned char> m;
    append_header(m, 'A', 1);
    append_be64(m, ref);
    m.push_back(static_cast<unsigned char>(side));
    append_be32(m, shares);
    const char sym[8] = {'T', 'E', 'S', 'T', ' ', ' ', ' ', ' '};
    m.insert(m.end(), sym, sym + 8);
    append_be32(m, price);
    return m;
}

// Order Executed: ref, shares, match number. Price comes from the book.
std::vector<unsigned char> execute(std::uint64_t ref, std::uint32_t shares, std::uint64_t match) {
    std::vector<unsigned char> m;
    append_header(m, 'E', 1);
    append_be64(m, ref);
    append_be32(m, shares);
    append_be64(m, match);
    return m;
}

// Order Executed With Price: ref, shares, match, printable, wire price.
std::vector<unsigned char> execute_with_price(std::uint64_t ref, std::uint32_t shares,
                                              std::uint64_t match, bool pr, std::uint32_t px) {
    std::vector<unsigned char> m;
    append_header(m, 'C', 1);
    append_be64(m, ref);
    append_be32(m, shares);
    append_be64(m, match);
    m.push_back(pr ? 'Y' : 'N');
    append_be32(m, px);
    return m;
}

std::vector<unsigned char> order_delete(std::uint64_t ref) {
    std::vector<unsigned char> m;
    append_header(m, 'D', 1);
    append_be64(m, ref);
    return m;
}

std::vector<unsigned char> join(const std::vector<std::vector<unsigned char>>& parts) {
    std::vector<unsigned char> out;
    for (const auto& p : parts)
        out.insert(out.end(), p.begin(), p.end());
    return out;
}

mog::replay::Options raw_opts() {
    mog::replay::Options o;
    o.format = mog::replay::Format::itch_raw;
    return o;
}

mog::trades::Summary run(const std::vector<unsigned char>& bytes,
                         const mog::replay::Options& opts) {
    auto r = mog::trades::run_trades(bytes, opts);
    CHECK(r.has_value());
    return r.has_value() ? *r : mog::trades::Summary{};
}

} // namespace

int main() {
    // Stream:
    //   A  ref=10 bid 1000 @ 100000          ts 1000000
    //   E  ref=10 exec 400 match=77          -> print (77, level 100000, 400)
    //   C  ref=10 exec 300 match=78 @100005  -> print (78, wire 100005, 300)
    //   D  ref=10
    //   A  ref=11 ask 500 @ 101000
    //   E  ref=11 exec 200 match=79          -> print (79, level 101000, 200)
    //   E  ref=99 exec 123 match=80          -> unpriced (unknown ref), no print
    //   C  ref=11 exec 100 match=81 @99900 N -> print (81, wire 99900, 100)
    // Hand-computed expectation: 4 prints; matches 77,78,79,81 in stream order.
    const auto stream = join({
        add_order('B', 10, 1000, 100000),
        execute(10, 400, 77),
        execute_with_price(10, 300, 78, true, 100005),
        order_delete(10),
        add_order('S', 11, 500, 101000),
        execute(11, 200, 79),
        execute(99, 123, 80), // unknown ref
        execute_with_price(11, 100, 81, false, 99900),
    });

    const auto t = run(stream, raw_opts());
    CHECK(t.prints == 4);
    CHECK(t.unpriced_executions == 1);
    CHECK(t.book_events_failed == 1); // only the unknown-ref execution fails
    CHECK(t.decoded == 8);
    CHECK(t.skipped == 0);
    CHECK(t.records.size() == 4);

    if (t.records.size() == 4) {
        const auto& r0 = t.records[0];
        CHECK(r0.match_number == 77);
        CHECK(r0.price_ticks == 100000); // level price recovered from the book
        CHECK(r0.shares == 400);
        CHECK(!r0.from_execute_with_price);
        CHECK(r0.printable);
        CHECK(r0.locate == 1);
        CHECK(r0.ts_ns != 0);

        const auto& r1 = t.records[1];
        CHECK(r1.match_number == 78);
        CHECK(r1.price_ticks == 100005); // C carries its own price by contract
        CHECK(r1.shares == 300);
        CHECK(r1.from_execute_with_price);
        CHECK(r1.printable);

        const auto& r2 = t.records[2];
        CHECK(r2.match_number == 79);
        CHECK(r2.price_ticks == 101000);
        CHECK(r2.shares == 200);

        const auto& r3 = t.records[3];
        CHECK(r3.match_number == 81);
        CHECK(r3.price_ticks == 99900);
        CHECK(r3.shares == 100);
        CHECK(!r3.printable); // printable flag carried through, not assumed

        // Timestamps strictly increasing in stream order.
        CHECK(r0.ts_ns < r1.ts_ns && r1.ts_ns < r2.ts_ns && r2.ts_ns < r3.ts_ns);
    }

    // Canonical CSV rendering is exact.
    if (t.records.size() == 4) {
        const std::string line0 = mog::trades::to_csv(t.records[0]);
        const std::string expect0 =
            "77,1," + std::to_string(t.records[0].ts_ns) + ",100000,400,Y,N";
        CHECK(line0 == expect0);
        const std::string line3 = mog::trades::to_csv(t.records[3]);
        const std::string expect3 = "81,1," + std::to_string(t.records[3].ts_ns) + ",99900,100,N,Y";
        CHECK(line3 == expect3);
        CHECK(mog::trades::csv_header() == "match_number,locate,ts_ns,price_ticks,shares,printable,"
                                           "with_price");
    }

    // MoldUDP64 framing yields byte-identical records to the raw path.
    {
        std::vector<std::vector<unsigned char>> payloads;
        std::size_t off = 0;
        while (off < stream.size()) {
            const std::size_t len = mog::kFrame.length[stream[off]];
            payloads.emplace_back(stream.begin() + static_cast<long>(off),
                                  stream.begin() + static_cast<long>(off + len));
            off += len;
        }
        std::vector<unsigned char> framed(mog::mold::kPacketHeaderSize);
        std::memcpy(framed.data(), "TRADES001", mog::mold::kSessionSize);
        mog::wire::store_be64(framed.data() + mog::mold::kSessionSize, 1);
        mog::wire::store_be16(framed.data() + mog::mold::kSessionSize + 8,
                              static_cast<std::uint16_t>(payloads.size()));
        for (const auto& pl : payloads) {
            append_be16(framed, static_cast<std::uint16_t>(pl.size()));
            framed.insert(framed.end(), pl.begin(), pl.end());
        }
        const auto tm = run(framed, [&] {
            mog::replay::Options o;
            o.format = mog::replay::Format::moldudp64;
            return o;
        }());
        CHECK(tm.prints == t.prints);
        CHECK(tm.unpriced_executions == t.unpriced_executions);
        CHECK(tm.records.size() == t.records.size());
        if (tm.records.size() == t.records.size())
            for (std::size_t i = 0; i < tm.records.size(); ++i) {
                CHECK(tm.records[i].match_number == t.records[i].match_number);
                CHECK(tm.records[i].price_ticks == t.records[i].price_ticks);
                CHECK(tm.records[i].shares == t.records[i].shares);
                CHECK(tm.records[i].ts_ns == t.records[i].ts_ns);
            }
    }

    // Canonical CSV diff: clean pass, field mismatches, coverage gaps.
    {
        const std::string base = "match_number,locate,ts_ns,price_ticks,shares,printable,"
                                 "with_price\n"
                                 "77,1,2,100000,400,Y,N\n"
                                 "78,1,3,100005,300,Y,Y\n";
        mog::trades::ParsedCsv a, b;
        CHECK(mog::trades::parse_canonical_csv(base, a));
        CHECK(mog::trades::parse_canonical_csv(base, b));
        const auto clean = mog::trades::diff(a, b);
        CHECK(clean.clean());

        const std::string mangled = "match_number,locate,ts_ns,price_ticks,shares,printable,"
                                    "with_price\n"
                                    "77,1,2,100000,401,Y,N\n" // shares differ
                                    "79,1,4,101000,500,Y,N\n" // only in reference
                                    "80,1,5,100001,10,Y,N\n"; // only in reference
        mog::trades::ParsedCsv c;
        CHECK(mog::trades::parse_canonical_csv(mangled, c));
        const auto d = mog::trades::diff(a, c);
        CHECK(!d.clean());
        CHECK(d.rows_ours == 2);
        CHECK(d.rows_ref == 3);
        CHECK(d.matched == 1);
        CHECK(d.shares_mismatch == 1);
        CHECK(d.price_mismatch == 0);
        CHECK(d.has_divergence);
        CHECK(d.first_divergence.match_number == 77);
        CHECK(d.first_divergence.ours_shares == 400);
        CHECK(d.first_divergence.ref_shares == 401);

        // Non-canonical content is rejected, not silently accepted.
        mog::trades::ParsedCsv junk;
        CHECK(!mog::trades::parse_canonical_csv("nope\n", junk));
    }

    // Regression (O2): the old 12M-tick default band silently rejected every
    // order above $1200, so AMZN-class names reconstructed zero prints and
    // their executions surfaced as mystery "unpriced" counts. Defaults now
    // span the full ITCH wire price domain.
    {
        g_ts = 0;
        const std::vector<unsigned char> bytes =
            join({add_order('B', 1, 100, 20'000'000), // $2000.00 - above any $1200 cap
                  execute(1, 100, 900)});
        const auto wide = run(bytes, raw_opts());
        CHECK(wide.prints == 1);
        CHECK(wide.unpriced_executions == 0);
        CHECK(!wide.records.empty() && wide.records[0].price_ticks == 20'000'000);
    }

    if (failures == 0)
        std::printf("trades_e2e: ok\n");
    else
        std::printf("trades_e2e: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
