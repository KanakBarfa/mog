// MoldUDP64 downstream packet framing with sequence-continuity enforcement.
// A silently dropped packet corrupts an entire replayed day; every anomaly
// here is a loud typed error carrying session, expected/got sequence, and the
// byte offset - never silent truncation.
//
// Scope: framing and continuity only. Payload interpretation (which ITCH
// types are decodable vs skippable) belongs to the ITCH layer above.
#pragma once

#include <mog/Wire.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace mog::mold {

inline constexpr std::size_t kSessionSize = 10;
inline constexpr std::size_t kPacketHeaderSize = kSessionSize + 8 + 2;
// Message Count sentinels per the MoldUDP64 1.00 specification.
inline constexpr std::uint16_t kHeartbeatCount = 0xFFFF;
inline constexpr std::uint16_t kEndSessionCount = 0x0000;

enum class Error : std::uint8_t {
    truncated_header = 1, // fewer than 20 bytes where a packet header must be
    truncated_message,    // length prefix or payload runs past end of stream
    bad_message_length,   // zero-length message block
    sequence_gap,         // packet starts ahead of the expected sequence
    sequence_regression,  // packet starts behind the expected sequence
    trailing_bytes,       // data after end-of-session marker
};

struct GapInfo {
    std::uint64_t expected_seq = 0;
    std::uint64_t got_seq = 0;
    std::size_t offset = 0; // byte offset of the offending packet header
    char session[kSessionSize] = {};
};

struct ErrorInfo {
    Error code;
    std::size_t offset = 0; // byte offset where the error was detected
    GapInfo gap{};          // meaningful only when code == sequence_gap/regression
};

[[nodiscard]] constexpr std::string_view error_text(Error e) noexcept {
    switch (e) {
    case Error::truncated_header:
        return "truncated_header";
    case Error::truncated_message:
        return "truncated_message";
    case Error::bad_message_length:
        return "bad_message_length";
    case Error::sequence_gap:
        return "sequence_gap";
    case Error::sequence_regression:
        return "sequence_regression";
    case Error::trailing_bytes:
        return "trailing_bytes";
    }
    return "?";
}

// Raw packet view: session string, first message sequence number, count.
struct PacketView {
    std::string_view session;
    std::uint64_t sequence = 0;
    std::uint16_t count = 0;
};

[[nodiscard]] inline PacketView load_packet_header(const unsigned char* p) noexcept {
    return PacketView{
        .session = {reinterpret_cast<const char*>(p), kSessionSize},
        .sequence = wire::load_be64(p + kSessionSize),
        .count = wire::load_be16(p + kSessionSize + 8),
    };
}

namespace detail {
[[nodiscard]] inline GapInfo make_gap(std::uint64_t expected, std::uint64_t got, std::size_t off,
                                      std::string_view session) noexcept {
    GapInfo g{expected, got, off, {}};
    const std::size_t n = session.size() < kSessionSize ? session.size() : kSessionSize;
    for (std::size_t i = 0; i < n; ++i)
        g.session[i] = session[i];
    return g;
}
} // namespace detail

// Continuity-checked framing over [buf, buf+len). For every carried message,
// sink(payload_ptr, payload_len, sequence_number) is invoked in stream order.
//
// Continuity rules:
//   - The first packet establishes the session's starting sequence.
//   - A later packet whose start sequence is ahead of the running expectation
//     is a gap; behind it is a regression. Both are hard errors.
//   - Heartbeats (count 0xFFFF) carry the next expected sequence and must
//     match it exactly.
//   - End-of-session (count 0x0000) terminates the stream; any bytes after
//     it are an error rather than ignored garbage.
template <typename Sink>
    requires requires(Sink s, const unsigned char* p, std::size_t n, std::uint64_t seq) {
        s(p, n, seq);
    }
[[nodiscard]] std::expected<std::size_t, ErrorInfo>
parse_moldudp64(const unsigned char* buf, std::size_t len, Sink& sink) noexcept {
    std::size_t off = 0;
    std::uint64_t expected_seq = 0;
    char established_session[kSessionSize] = {};
    bool established = false;

    while (off < len) {
        if (len - off < kPacketHeaderSize)
            return std::unexpected(ErrorInfo{Error::truncated_header, off});
        const std::size_t pkt_off = off;
        const PacketView hdr = load_packet_header(buf + off);
        off += kPacketHeaderSize;

        if (!established) {
            established = true;
            expected_seq = hdr.sequence;
            const std::size_t n =
                hdr.session.size() < kSessionSize ? hdr.session.size() : kSessionSize;
            for (std::size_t i = 0; i < n; ++i)
                established_session[i] = hdr.session[i];
        } else if (hdr.count != kEndSessionCount) {
            const std::string_view est_view{established_session, kSessionSize};
            if (hdr.session != est_view) {
                // Session failover restarts sequencing, even on heartbeats.
                expected_seq = hdr.sequence;
                const std::size_t n =
                    hdr.session.size() < kSessionSize ? hdr.session.size() : kSessionSize;
                for (std::size_t i = 0; i < n; ++i)
                    established_session[i] = hdr.session[i];
            } else if (hdr.sequence != expected_seq && hdr.count != kHeartbeatCount) {
                return std::unexpected(ErrorInfo{
                    hdr.sequence > expected_seq ? Error::sequence_gap : Error::sequence_regression,
                    pkt_off, detail::make_gap(expected_seq, hdr.sequence, pkt_off, hdr.session)});
            }
        }

        if (hdr.count == kEndSessionCount) {
            if (off != len)
                return std::unexpected(ErrorInfo{Error::trailing_bytes, off});
            return off;
        }
        if (hdr.count == kHeartbeatCount) {
            if (hdr.sequence != expected_seq)
                return std::unexpected(ErrorInfo{
                    hdr.sequence > expected_seq ? Error::sequence_gap : Error::sequence_regression,
                    pkt_off, detail::make_gap(expected_seq, hdr.sequence, pkt_off, hdr.session)});
            continue;
        }

        for (std::uint16_t i = 0; i < hdr.count; ++i) {
            if (len - off < 2)
                return std::unexpected(ErrorInfo{Error::truncated_message, off});
            const std::uint16_t msg_len = wire::load_be16(buf + off);
            off += 2;
            if (msg_len == 0)
                return std::unexpected(ErrorInfo{Error::bad_message_length, off});
            if (len - off < msg_len)
                return std::unexpected(ErrorInfo{Error::truncated_message, off});
            sink(buf + off, static_cast<std::size_t>(msg_len), expected_seq);
            ++expected_seq;
            off += msg_len;
        }
    }
    return off;
}

} // namespace mog::mold
