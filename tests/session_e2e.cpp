// Session modeling (G3): phases, halt windows, NOII-driven close cross -
// built from exact ITCH 5.0 frames with hand-computed expectations.

#include <mog/Replay.hpp>
#include <mog/Session.hpp>
#include <mog/SessionFeed.hpp>

#include <cstdio>
#include <cstring>
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

void append_be(std::vector<unsigned char>& v, std::uint64_t x, int bytes) {
    unsigned char b[8];
    if (bytes == 4)
        mog::wire::store_be32(b, static_cast<std::uint32_t>(x));
    else if (bytes == 6)
        mog::wire::store_be48(b, x);
    else
        mog::wire::store_be64(b, x);
    v.insert(v.end(), b, b + bytes);
}

std::vector<unsigned char> system_event(char code, std::uint64_t ts) {
    std::vector<unsigned char> m;
    m.push_back(static_cast<unsigned char>('S'));
    append_be(m, 1, 2); // locate
    append_be(m, 7, 2); // tracking
    append_be(m, ts, 6);
    m.push_back(static_cast<unsigned char>(code));
    return m;
}

std::vector<unsigned char> trading_action(char state, const char* reason, std::uint64_t ts,
                                          std::uint32_t locate = 1) {
    std::vector<unsigned char> m;
    m.push_back(static_cast<unsigned char>('H'));
    append_be(m, locate, 2);
    append_be(m, 7, 2);
    append_be(m, ts, 6);
    for (int i = 0; i < 8; ++i)
        m.push_back('A');
    m.push_back(static_cast<unsigned char>(state));
    m.push_back(' ');
    for (int i = 0; i < 4; ++i)
        m.push_back(static_cast<unsigned char>(reason[i]));
    return m;
}

std::vector<unsigned char> noii(std::uint64_t ts, std::uint32_t locate, std::uint64_t paired,
                                std::uint64_t imbalance, char dir, std::uint32_t near_px,
                                std::uint32_t ref_px) {
    std::vector<unsigned char> m;
    m.push_back(static_cast<unsigned char>('I'));
    append_be(m, locate, 2);
    append_be(m, 7, 2);
    append_be(m, ts, 6);
    append_be(m, paired, 8);
    append_be(m, imbalance, 8);
    m.push_back(static_cast<unsigned char>(dir));
    for (int i = 0; i < 8; ++i)
        m.push_back('A');
    append_be(m, near_px + 100000, 4); // far price
    append_be(m, near_px, 4);          // near reference price
    append_be(m, ref_px, 4);           // current reference
    m.push_back(static_cast<unsigned char>('O'));
    m.push_back(' ');
    return m;
}

std::vector<unsigned char> reg_sho(char action, std::uint64_t ts, std::uint32_t locate = 1) {
    std::vector<unsigned char> m;
    m.push_back(static_cast<unsigned char>('Y'));
    append_be(m, locate, 2);
    append_be(m, 7, 2);
    append_be(m, ts, 6);
    for (int i = 0; i < 8; ++i)
        m.push_back('A');
    m.push_back(static_cast<unsigned char>(action));
    return m;
}

std::vector<unsigned char> add_order(char side, std::uint64_t ref, std::uint32_t shares,
                                     std::uint32_t px) { // 36 bytes
    std::vector<unsigned char> m;
    m.push_back(static_cast<unsigned char>('A'));
    append_be(m, 1, 2);
    append_be(m, 7, 2);
    append_be(m, 34'200'000'000ULL, 6); // 09:30:00.000000008-ish
    append_be(m, ref, 8);
    m.push_back(static_cast<unsigned char>(side));
    append_be(m, shares, 4);
    for (int i = 0; i < 8; ++i)
        m.push_back('A');
    append_be(m, px, 4);
    return m;
}

std::vector<unsigned char> execute(std::uint64_t ref, std::uint32_t shares,
                                   std::uint64_t match) { // 31 bytes
    std::vector<unsigned char> m;
    m.push_back(static_cast<unsigned char>('E'));
    append_be(m, 1, 2);
    append_be(m, 7, 2);
    append_be(m, 34'201'000'000ULL, 6);
    append_be(m, ref, 8);
    append_be(m, shares, 4);
    append_be(m, match, 8);
    return m;
}

} // namespace

int main() {
    // Direct model checks first - decode-independent semantics.
    mog::session::Model model;
    CHECK(model.phase() == mog::session::Phase::idle);
    model.on_system_event('Q', 100);
    CHECK(model.phase() == mog::session::Phase::market_open);
    model.on_trading_action(1, 'H', "TST ", 200);
    CHECK(model.is_halted(1));
    CHECK(!model.is_halted(2));
    CHECK(model.was_halted_at(1, 250));
    model.on_trading_action(1, 'T', "    ", 300);
    CHECK(!model.is_halted(1));
    CHECK(model.was_halted_at(1, 250)); // inside the now-closed window
    CHECK(!model.was_halted_at(1, 350));
    CHECK(model.halt_windows().size() == 1);
    CHECK(model.halt_windows()[0].end_ns == 300);

    // Close-cross computation from latest NOII per locate.
    mog::session::NoiiSnapshot a;
    a.locate = 1;
    a.ts_ns = 400;
    a.paired_shares = 5000;
    a.imbalance_shares = 200;
    a.direction = 'B';
    a.near_price_ticks = 1000000;
    a.reference_price_ticks = 999999;
    model.on_noii(a);
    // A later snapshot for the same locate replaces the earlier one.
    mog::session::NoiiSnapshot b = a;
    b.imbalance_shares = 700;
    b.ts_ns = 500;
    model.on_noii(b);
    mog::session::NoiiSnapshot c = a;
    c.locate = 2;
    c.paired_shares = 12000;
    c.imbalance_shares = 0;
    c.direction = 'S';
    c.near_price_ticks = 555000;
    model.on_noii(c);
    model.on_system_event('M', 600);
    CHECK(model.phase() == mog::session::Phase::market_closed);
    CHECK(model.close_crosses().size() == 2);
    if (model.close_crosses().size() == 2) {
        const auto& r1 = model.close_crosses()[0];
        CHECK(r1.locate == 1);
        CHECK(r1.qty == 5700); // paired 5000 + latest imbalance 700
        CHECK(r1.price_ticks == 1000000);
        CHECK(r1.direction == 'B');
        const auto& r2 = model.close_crosses()[1];
        CHECK(r2.qty == 12000); // zero imbalance contributes nothing
        CHECK(r2.price_ticks == 555000);
    }

    // End-to-end through run_replay: session frames interleaved with live
    // order flow; an execution DURING the halt window is countable.
    const auto stream = [&] {
        std::vector<std::vector<unsigned char>> frames;
        frames.push_back(system_event('O', 34'000'000'000ULL));
        frames.push_back(system_event('S', 34'010'000'000ULL));
        frames.push_back(system_event('Q', 34'200'000'000ULL));
        frames.push_back(add_order('B', 10, 1000, 100000));
        frames.push_back(execute(10, 400, 77)); // continuous-session print
        frames.push_back(trading_action('H', "LUDP", 34'300'000'000ULL));
        frames.push_back(execute(10, 100, 78)); // print during halt!
        frames.push_back(trading_action('T', "    ", 34'400'000'000ULL));
        frames.push_back(reg_sho('1', 34'450'000'000ULL));
        frames.push_back(noii(57'500'000'000ULL, 1, 5000, 700, 'B', 1000000, 999999));
        frames.push_back(system_event('M', 57'600'000'000ULL));
        return frames;
    }();
    std::vector<unsigned char> bytes;
    for (const auto& f : stream)
        bytes.insert(bytes.end(), f.begin(), f.end());

    const auto r = mog::replay::run_replay(bytes, {});
    CHECK(r.has_value());
    if (r) {
        CHECK(r->decoded > 0);  // subset messages still decoded
        CHECK(r->skipped == 8); // S,O+S+Q+M, H,H, Y, I
        CHECK(r->phase == mog::session::Phase::market_closed);
        CHECK(r->halt_windows == 1);
        CHECK(r->noii_updates == 1);
        CHECK(r->close_crosses.size() == 1);
        if (r->close_crosses.size() == 1) {
            CHECK(r->close_crosses[0].qty == 5700);
            CHECK(r->close_crosses[0].price_ticks == 1000000);
        }
    }

    if (failures == 0)
        std::printf("session_e2e: ok\n");
    else
        std::printf("session_e2e: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
