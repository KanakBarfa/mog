// Raw standard-frame decoding for the session model: field offsets for
// System Event (S), Trading Action (H), NOII (I) and Reg SHO (Y) per the
// ITCH 5.0 layout used by kSessionFrame. Feeds a session::Model directly
// from skipped frames - no Message union involvement.
#pragma once

#include <mog/Session.hpp>

namespace mog::session::detail {

[[nodiscard]] inline std::uint64_t frame_ts(const unsigned char* p) noexcept {
    return wire::load_be48(p + 5);
}

[[nodiscard]] inline bool feed_frame(Model& model, char type, const unsigned char* p,
                                     std::size_t len) noexcept {
    switch (type) {
    case 'S': { // 12 bytes: header + event code
        if (len < 12)
            return false;
        model.on_system_event(static_cast<char>(p[11]), frame_ts(p));
        return true;
    }
    case 'H': { // 25: header + stock(8) + state(1) + reserved(1) + reason(4)
        if (len < 25)
            return false;
        const std::uint32_t locate = wire::load_be16(p + 1);
        model.on_trading_action(locate, static_cast<char>(p[19]),
                                reinterpret_cast<const char*>(p + 21), frame_ts(p));
        return true;
    }
    case 'I': { // 50: header + paired(8) + imbalance(8) + dir(1) + stock(8)
                //      + far(4) + near(4) + ref(4) + cross type(1) + variation(1)
        if (len < 50)
            return false;
        NoiiSnapshot s;
        s.ts_ns = frame_ts(p);
        s.locate = wire::load_be16(p + 1);
        s.paired_shares = wire::load_be64(p + 11);
        s.imbalance_shares = wire::load_be64(p + 19);
        s.direction = static_cast<char>(p[27]);
        s.near_price_ticks = static_cast<std::int64_t>(wire::load_be32(p + 40));
        s.reference_price_ticks = static_cast<std::int64_t>(wire::load_be32(p + 44));
        model.on_noii(s);
        return true;
    }
    case 'Y': { // 20: header + stock(8) + action code(1)
        if (len < 20)
            return false;
        const std::uint32_t locate = wire::load_be16(p + 1);
        model.on_reg_sho(locate, static_cast<char>(p[19]));
        return true;
    }
    default:
        return false; // not a session-relevant frame; caller may ignore
    }
}

} // namespace mog::session::detail
