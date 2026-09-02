// Zero-copy PCAP / Ethernet-IPv4-UDP market data packet stream ingestion.
#pragma once

#include <mog/Contracts.hpp>
#include <mog/Types.hpp>
#include <mog/Wire.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace mog::pcap {

inline constexpr std::uint32_t kPcapMagicMicroBE = 0xa1b2c3d4;
inline constexpr std::uint32_t kPcapMagicMicroLE = 0xd4c3b2a1;
inline constexpr std::uint32_t kPcapMagicNanoBE = 0xa1b23c4d;
inline constexpr std::uint32_t kPcapMagicNanoLE = 0x4d3cb2a1;

inline constexpr std::uint32_t kLinkTypeEthernet = 1;
inline constexpr std::uint32_t kLinkTypeNull = 0;
inline constexpr std::uint32_t kLinkTypeRaw = 101;

struct PacketView {
    std::uint64_t ts_ns = 0;
    std::span<const std::uint8_t> raw_packet{};
    std::span<const std::uint8_t> payload{};
    std::uint16_t src_port = 0;
    std::uint16_t dst_port = 0;
    std::uint8_t protocol = 0; // 17 = UDP, 6 = TCP
};

class PcapReader {
public:
    explicit PcapReader(std::span<const std::uint8_t> buffer) noexcept : buf_(buffer) {
        parse_header();
    }

    [[nodiscard]] bool is_valid() const noexcept { return valid_; }
    [[nodiscard]] bool is_nanosecond() const noexcept { return is_nanosec_; }
    [[nodiscard]] std::uint32_t link_type() const noexcept { return link_type_; }

    template <typename PacketFn>
    std::size_t for_each_packet(PacketFn&& fn) const {
        if (!valid_)
            return 0;

        std::size_t offset = 24; // size of global header
        std::size_t packet_count = 0;

        while (offset + 16 <= buf_.size()) {
            const std::uint8_t* p = buf_.data() + offset;
            const std::uint32_t ts_sec = read32(p);
            const std::uint32_t ts_sub = read32(p + 4);
            const std::uint32_t incl_len = read32(p + 8);

            offset += 16;
            if (offset + incl_len > buf_.size())
                break;

            const std::uint64_t ts_ns =
                static_cast<std::uint64_t>(ts_sec) * 1'000'000'000ULL +
                (is_nanosec_ ? static_cast<std::uint64_t>(ts_sub)
                             : static_cast<std::uint64_t>(ts_sub) * 1'000ULL);

            const std::span<const std::uint8_t> raw(buf_.data() + offset, incl_len);
            PacketView view = extract_payload(raw, ts_ns);

            fn(view);
            ++packet_count;
            offset += incl_len;
        }

        return packet_count;
    }

private:
    void parse_header() noexcept {
        if (buf_.size() < 24) {
            valid_ = false;
            return;
        }
        const std::uint32_t magic = wire::load_be32(buf_.data());
        if (magic == kPcapMagicMicroBE) {
            is_swapped_ = false;
            is_nanosec_ = false;
            valid_ = true;
        } else if (magic == kPcapMagicNanoBE) {
            is_swapped_ = false;
            is_nanosec_ = true;
            valid_ = true;
        } else {
            const std::uint32_t magic_le = wire::load_le32(buf_.data());
            if (magic_le == kPcapMagicMicroBE) {
                is_swapped_ = true;
                is_nanosec_ = false;
                valid_ = true;
            } else if (magic_le == kPcapMagicNanoBE) {
                is_swapped_ = true;
                is_nanosec_ = true;
                valid_ = true;
            } else {
                valid_ = false;
                return;
            }
        }
        link_type_ = read32(buf_.data() + 20);
    }

    [[nodiscard]] std::uint32_t read32(const std::uint8_t* p) const noexcept {
        return is_swapped_ ? wire::load_le32(p) : wire::load_be32(p);
    }

    [[nodiscard]] PacketView extract_payload(std::span<const std::uint8_t> raw,
                                             std::uint64_t ts_ns) const noexcept {
        PacketView view;
        view.ts_ns = ts_ns;
        view.raw_packet = raw;

        if (link_type_ != kLinkTypeEthernet || raw.size() < 14) {
            view.payload = raw;
            return view;
        }

        std::size_t l3_offset = 14;
        std::uint16_t ether_type = wire::load_be16(raw.data() + 12);
        if (ether_type == 0x8100 && raw.size() >= 18) { // 802.1Q VLAN
            ether_type = wire::load_be16(raw.data() + 16);
            l3_offset = 18;
        }

        if (ether_type != 0x0800 || raw.size() < l3_offset + 20) { // IPv4
            view.payload = raw.subspan(l3_offset);
            return view;
        }

        const std::uint8_t* ip = raw.data() + l3_offset;
        const std::size_t ip_hdr_len = (ip[0] & 0x0F) * 4;
        if (ip_hdr_len < 20 || raw.size() < l3_offset + ip_hdr_len) {
            view.payload = raw.subspan(l3_offset);
            return view;
        }

        view.protocol = ip[9];
        const std::size_t l4_offset = l3_offset + ip_hdr_len;

        if (view.protocol == 17 && raw.size() >= l4_offset + 8) { // UDP
            const std::uint8_t* udp = raw.data() + l4_offset;
            view.src_port = wire::load_be16(udp);
            view.dst_port = wire::load_be16(udp + 2);
            const std::size_t udp_len = wire::load_be16(udp + 4);
            const std::size_t payload_len = (udp_len >= 8 && l4_offset + udp_len <= raw.size())
                                                ? (udp_len - 8)
                                                : (raw.size() - l4_offset - 8);
            view.payload = raw.subspan(l4_offset + 8, payload_len);
        } else {
            view.payload = raw.subspan(l4_offset);
        }

        return view;
    }

    std::span<const std::uint8_t> buf_;
    bool valid_ = false;
    bool is_swapped_ = false;
    bool is_nanosec_ = false;
    std::uint32_t link_type_ = kLinkTypeEthernet;
};

} // namespace mog::pcap
