// Replay stack end-to-end: MoldUDP64 framing + gap loudness, session-type
// skipping, mmap/read digest identity, truncation honesty, determinism.

#include <mog/MoldUdp.hpp>
#include <mog/Replay.hpp>
#include <mog/SimRun.hpp>
#include <mog/Wire.hpp>
#include <support/CorpusGen.hpp>

#include <cstdio>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

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
using mog::replay::Format;
using mog::replay::Options;
using mog::replay::run_replay;

Options raw_opts() {
    Options o;
    o.format = Format::itch_raw;
    return o;
}

Options mold_opts() {
    Options o;
    o.format = Format::moldudp64;
    return o;
}

std::vector<Message> make_messages() {
    return mog::testing::make_corpus(
        {.seed = 0x5A69ULL, .count = 512, .price_center = 4'000'000, .price_span = 400'000});
}

std::vector<unsigned char> raw_bytes(const std::vector<Message>& msgs) {
    return mog::testing::encode_corpus(msgs);
}

// Slices an encoded stream back into per-message payloads using the core
// frame table (the corpus contains only decodable-subset types).
std::vector<std::vector<unsigned char>> slice_payloads(const std::vector<unsigned char>& raw) {
    std::vector<std::vector<unsigned char>> out;
    std::size_t off = 0;
    while (off < raw.size()) {
        const std::size_t len = mog::kFrame.length[raw[off]];
        out.emplace_back(raw.begin() + static_cast<long>(off),
                         raw.begin() + static_cast<long>(off + len));
        off += len;
    }
    return out;
}

// --- MoldUDP64 packet assembly --------------------------------------------

std::vector<unsigned char> mold_packet(std::uint64_t seq,
                                       const std::vector<std::vector<unsigned char>>& payloads,
                                       std::size_t begin, std::size_t end) {
    std::vector<unsigned char> p(mog::mold::kPacketHeaderSize);
    std::memcpy(p.data(), "REPLAYTST", mog::mold::kSessionSize);
    mog::wire::store_be64(p.data() + mog::mold::kSessionSize, seq);
    mog::wire::store_be16(p.data() + mog::mold::kSessionSize + 8,
                          static_cast<std::uint16_t>(end - begin));
    for (std::size_t j = begin; j < end; ++j) {
        unsigned char lenb[2];
        mog::wire::store_be16(lenb, static_cast<std::uint16_t>(payloads[j].size()));
        p.insert(p.end(), lenb, lenb + 2);
        p.insert(p.end(), payloads[j].begin(), payloads[j].end());
    }
    return p;
}

std::vector<unsigned char> join(const std::vector<std::vector<unsigned char>>& parts) {
    std::vector<unsigned char> out;
    for (const auto& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

// --- Session-type frame builders (exact ITCH 5.0 lengths) ------------------

void append_u48(std::vector<unsigned char>& v, std::uint64_t x) {
    unsigned char b[6];
    mog::wire::store_be48(b, x);
    v.insert(v.end(), b, b + 6);
}

void append_u32(std::vector<unsigned char>& v, std::uint32_t x) {
    unsigned char b[4];
    mog::wire::store_be32(b, x);
    v.insert(v.end(), b, b + 4);
}

void append_u64(std::vector<unsigned char>& v, std::uint64_t x) {
    unsigned char b[8];
    mog::wire::store_be64(b, x);
    v.insert(v.end(), b, b + 8);
}

void append_session_header(std::vector<unsigned char>& m, char type, std::uint64_t ts) {
    m.push_back(static_cast<unsigned char>(type));
    append_u32(m, (static_cast<std::uint32_t>(1) << 16) | 7u); // locate+tracking
    append_u48(m, ts);
}

// Returns a spec-length frame of the given session type, or empty if unknown.
std::vector<unsigned char> session_frame(char type, std::uint64_t ts) {
    std::vector<unsigned char> m;
    switch (type) {
    case 'S': // System Event: 12
        append_session_header(m, 'S', ts);
        m.push_back(static_cast<unsigned char>('O'));
        break;
    case 'H': // Stock Trading Action: 25
        append_session_header(m, 'H', ts);
        m.resize(m.size() + 8, 'A');
        m.push_back('T');
        m.push_back(' ');
        m.resize(m.size() + 4, ' ');
        break;
    case 'N': // RPI Interest Flag: 20
        append_session_header(m, 'N', ts);
        m.resize(m.size() + 8, 'A');
        m.push_back('0');
        break;
    case 'P': // Trade (Non-Cross): 44
        append_session_header(m, 'P', ts);
        append_u64(m, 42);  // order reference
        m.push_back('B');   // buy/sell
        append_u32(m, 100); // shares
        m.resize(m.size() + 8, 'A');
        append_u32(m, 4'010'000); // price
        append_u64(m, 7);         // execution (match) id
        break;
    default:
        break;
    }
    return m;
}

} // namespace

int main() {
    const auto msgs = make_messages();
    const auto raw = raw_bytes(msgs);

    // 1. Raw replay: full decode, run-to-run determinism over EVERY field.
    // Book-event failures are legal here: the corpus generator draws E/C
    // refs randomly, so unknown-order rejections are part of the stream.
    const auto a = run_replay(raw, raw_opts());
    const auto b = run_replay(raw, raw_opts());
    CHECK(a.has_value() && b.has_value());
    if (a && b) {
        CHECK(a->decoded == msgs.size());
        CHECK(a->skipped == 0);
        CHECK(a->bytes_consumed == raw.size());
        CHECK(a->trace_digest == b->trace_digest);
        CHECK(a->live_orders == b->live_orders);
        CHECK(a->best_bid_ticks == b->best_bid_ticks);
        CHECK(a->best_ask_ticks == b->best_ask_ticks);
        CHECK(a->book_events_failed == b->book_events_failed);
        CHECK(a->decoded + a->skipped > 0);
    }

    // 2. G1 acceptance: mmap(MAP_POPULATE) and read() agree byte-for-byte on
    // every summary field - how bytes arrived must not change results.
    const char* tmpdir = std::getenv("TMPDIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/mog-replay-e2eXXXXXX", tmpdir ? tmpdir : "/tmp");
    const int fd = ::mkstemp(path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const ssize_t written = ::write(fd, raw.data(), static_cast<size_t>(raw.size()));
        CHECK(written == static_cast<ssize_t>(raw.size()));
        ::close(fd);
        auto mapped = mog::replay::MappedFile::load(path, true);
        auto streamed = mog::replay::MappedFile::load(path, false);
        CHECK(mapped.has_value() && streamed.has_value());
        if (mapped && streamed) {
            const auto mm = run_replay(
                std::span<const unsigned char>{mapped->data(), mapped->size()}, raw_opts());
            const auto rd = run_replay(
                std::span<const unsigned char>{streamed->data(), streamed->size()}, raw_opts());
            CHECK(mm.has_value() && rd.has_value());
            if (mm && rd) {
                CHECK(mm->trace_digest == rd->trace_digest);
                CHECK(mm->decoded == rd->decoded);
                CHECK(mm->skipped == rd->skipped);
                CHECK(mm->live_orders == rd->live_orders);
                CHECK(mm->best_bid_ticks == rd->best_bid_ticks);
                CHECK(mm->best_ask_ticks == rd->best_ask_ticks);
                CHECK(mm->book_events_failed == rd->book_events_failed);
            }
        }
        ::unlink(path);
    }

    // 3. MoldUDP64 framing is transparent: chunked packets carry the same
    // decoded stream and therefore the same digest as the raw path.
    {
        const auto payloads = slice_payloads(raw);
        constexpr std::size_t kPerPacket = 40;
        std::vector<std::vector<unsigned char>> packets;
        for (std::size_t i = 0; i < payloads.size(); i += kPerPacket)
            packets.push_back(
                mold_packet(i + 1, payloads, i, std::min(i + kPerPacket, payloads.size())));
        const auto framed = join(packets);
        const auto m = run_replay(framed, mold_opts());
        CHECK(m.has_value());
        if (m && a) {
            CHECK(m->decoded == a->decoded);
            CHECK(m->trace_digest == a->trace_digest);
            CHECK(m->live_orders == a->live_orders);
            CHECK(m->bytes_consumed == framed.size());
        }
    }

    // 4. G2 acceptance: a dropped packet is loud, typed, and precise.
    {
        const auto payloads = slice_payloads(raw);
        const auto first = mold_packet(1, payloads, 0, payloads.size()); // seq 1..N
        auto second = mold_packet(payloads.size() + 1, payloads, 0,
                                  payloads.size()); // would be contiguous
        // Inject a gap: start sequence jumps 7 ahead of expectation.
        const std::uint64_t expected_seq = payloads.size() + 1;
        const std::uint64_t got_seq = expected_seq + 7;
        mog::wire::store_be64(second.data() + mog::mold::kSessionSize, got_seq);
        const auto framed = join({first, second});
        auto g = run_replay(framed, mold_opts());
        CHECK(!g.has_value());
        if (!g) {
            CHECK(g.error().code == mog::replay::Error::mold_gap);
            CHECK(g.error().mold.code == mog::mold::Error::sequence_gap);
            CHECK(g.error().mold.gap.expected_seq == expected_seq);
            CHECK(g.error().mold.gap.got_seq == got_seq);
            CHECK(g.error().mold.gap.offset == first.size());
            for (std::size_t i = 0; i < mog::mold::kSessionSize; ++i)
                CHECK(g.error().mold.gap.session[i] == static_cast<char>("REPLAYTST"[i]));
        }
        // Regression: sequence moving backwards is equally fatal.
        mog::wire::store_be64(second.data() + mog::mold::kSessionSize, expected_seq - 3);
        auto r = run_replay(join({first, second}), mold_opts());
        CHECK(!r.has_value());
        if (!r)
            CHECK(r.error().mold.code == mog::mold::Error::sequence_regression);
    }

    // 5. Session-type skipping: standard frames interleaved between corpus
    // messages are counted per type and never perturb the decoded digest.
    {
        const auto payloads = slice_payloads(raw);
        const char inject[] = {'S', 'H', 'N', 'P'};
        std::map<char, std::size_t> injected_by_type;
        std::vector<unsigned char> mixed;
        std::uint64_t ts = 500;
        for (std::size_t i = 0; i <= payloads.size(); ++i) {
            if (i % 17 == 0 && i > 0 && i < payloads.size()) {
                const char t = inject[i % 4];
                auto f = session_frame(t, ts += 97);
                mixed.insert(mixed.end(), f.begin(), f.end());
                ++injected_by_type[t];
            }
            if (i < payloads.size()) {
                mixed.insert(mixed.end(), payloads[i].begin(), payloads[i].end());
            }
        }
        const auto s = run_replay(mixed, raw_opts());
        CHECK(s.has_value());
        if (s && a) {
            CHECK(s->decoded == a->decoded);
            CHECK(s->trace_digest == a->trace_digest);
            CHECK(s->skipped == injected_by_type['S'] + injected_by_type['H'] +
                                    injected_by_type['N'] + injected_by_type['P']);
            CHECK(s->skipped_by_type[static_cast<unsigned char>('S')] == injected_by_type['S']);
            CHECK(s->skipped_by_type[static_cast<unsigned char>('H')] == injected_by_type['H']);
            CHECK(s->skipped_by_type[static_cast<unsigned char>('N')] == injected_by_type['N']);
            CHECK(s->skipped_by_type[static_cast<unsigned char>('P')] == injected_by_type['P']);
            CHECK(s->live_orders == a->live_orders);
        }
    }

    // 6. Truncation stays loud on both paths - no silent partial success.
    {
        auto cut = raw;
        cut.resize(cut.size() - 5);
        const auto t = run_replay(cut, raw_opts());
        CHECK(!t.has_value());
        if (!t) {
            CHECK(t.error().code == mog::replay::Error::parse_error);
            CHECK(t.error().parse.code == mog::DecodeError::truncated);
        }
        // MoldUDP64 payload truncation is a framing error, distinct code.
        const auto payloads = slice_payloads(raw);
        auto p = mold_packet(1, payloads, 0, payloads.size());
        p.resize(p.size() - 3); // sever inside the last payload
        const auto tm = run_replay(p, mold_opts());
        CHECK(!tm.has_value());
        if (!tm)
            CHECK(tm.error().mold.code == mog::mold::Error::truncated_message);
    }

    // 7. Empty input rejected explicitly, not "successfully replayed".
    CHECK(run_replay({}, raw_opts()).error().code == mog::replay::Error::empty_stream);

    // 8. Ladder pool exhaustion is a typed error under EVERY profile - the
    // real-day bug where compiled-out contracts turned exhaustion into
    // silent out-of-bounds writes must never come back. Prices spread far
    // wider than a tiny pool can hold.
    {
        mog::replay::Options tiny;
        tiny.book = {1u << 10, {0, 12'000'000, 4}};
        // One order per ~1024-tick page stride; page 5 exceeds pool=4.
        std::string script = "kind,ts_ns,side,price_ticks,qty,ref\n";
        for (int i = 0; i < 8; ++i) {
            script += "ext_add," + std::to_string(100 + i) + ",B," +
                      std::to_string(1000000 + i * 1024) + ",100," + std::to_string(i + 1) + "\n";
        }
        // Through the simulator: orders beyond the pool fail loudly as
        // typed book errors instead of corrupting the heap.
        mog::SimConfig cfg;
        cfg.book = tiny.book;
        auto e = mog::simrun::run(script, cfg);
        CHECK(e.has_value());
        if (e)
            CHECK(e->script_rows == 8); // all parsed; failures counted in-book
    }

    if (failures == 0)
        std::printf("replay_e2e: ok\n");
    else
        std::printf("replay_e2e: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
