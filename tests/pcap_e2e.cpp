// E2E test for zero-copy PCAP packet capture playback.
#include <mog/Contracts.hpp>
#include <mog/ITCHParser.hpp>
#include <mog/Pcap.hpp>
#include <mog/Wire.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

struct MessageCollector {
    std::size_t count = 0;
    std::uint64_t last_ref = 0;

    void on_message(const mog::Message& m) noexcept {
        if (m.type == 'A') {
            ++count;
            last_ref = m.add_order.order_ref.value;
        }
    }
};

std::vector<std::uint8_t> build_test_pcap() {
    std::vector<std::uint8_t> pcap;

    // 1. Global Header (24 bytes, nanosecond resolution)
    pcap.resize(24, 0);
    mog::wire::store_be32(pcap.data(), mog::pcap::kPcapMagicNanoBE);
    mog::wire::store_be16(pcap.data() + 4, 2);      // Major
    mog::wire::store_be16(pcap.data() + 6, 4);      // Minor
    mog::wire::store_be32(pcap.data() + 16, 65535); // Snaplen
    mog::wire::store_be32(pcap.data() + 20, mog::pcap::kLinkTypeEthernet);

    // 2. Synthesize ITCH 5.0 'A' Add Order message payload
    // Type 'A' (1 byte), locate (2), tracking (2), ts (6), ref (8), side (1), shares (4), stock
    // (8), price (4) = 36 bytes
    std::vector<std::uint8_t> itch_msg(36, 0);
    itch_msg[0] = 'A';
    mog::wire::store_be16(itch_msg.data() + 1, 1);
    mog::wire::store_be16(itch_msg.data() + 3, 0);
    mog::wire::store_be48(itch_msg.data() + 5, 34200000000000ULL);
    mog::wire::store_be64(itch_msg.data() + 11, 1234567ULL);
    itch_msg[19] = 'B';
    mog::wire::store_be32(itch_msg.data() + 20, 100);
    std::memcpy(itch_msg.data() + 24, "AAPL    ", 8);
    mog::wire::store_be32(itch_msg.data() + 32, 1500000);

    // 3. Build Ethernet + IPv4 + UDP packet frame
    std::vector<std::uint8_t> packet;
    // Ethernet (14 bytes)
    packet.resize(14, 0);
    mog::wire::store_be16(packet.data() + 12, 0x0800); // IPv4

    // IPv4 (20 bytes)
    std::vector<std::uint8_t> ip(20, 0);
    ip[0] = 0x45; // Version 4, IHL 5
    const std::uint16_t total_ip_len = static_cast<std::uint16_t>(20 + 8 + itch_msg.size());
    mog::wire::store_be16(ip.data() + 2, total_ip_len);
    ip[9] = 17; // UDP
    packet.insert(packet.end(), ip.begin(), ip.end());

    // UDP (8 bytes)
    std::vector<std::uint8_t> udp(8, 0);
    mog::wire::store_be16(udp.data(), 12345);     // Src port
    mog::wire::store_be16(udp.data() + 2, 54321); // Dst port
    mog::wire::store_be16(udp.data() + 4, static_cast<std::uint16_t>(8 + itch_msg.size()));
    packet.insert(packet.end(), udp.begin(), udp.end());

    // Append ITCH payload
    packet.insert(packet.end(), itch_msg.begin(), itch_msg.end());

    // 4. Append PCAP Packet Header (16 bytes)
    std::vector<std::uint8_t> pkt_hdr(16, 0);
    mog::wire::store_be32(pkt_hdr.data(), 1600000000); // ts_sec
    mog::wire::store_be32(pkt_hdr.data() + 4, 500000); // ts_nsec
    mog::wire::store_be32(pkt_hdr.data() + 8, static_cast<std::uint32_t>(packet.size()));
    mog::wire::store_be32(pkt_hdr.data() + 12, static_cast<std::uint32_t>(packet.size()));

    pcap.insert(pcap.end(), pkt_hdr.begin(), pkt_hdr.end());
    pcap.insert(pcap.end(), packet.begin(), packet.end());

    return pcap;
}

void test_pcap_decoding() {
    const auto pcap_data = build_test_pcap();
    mog::pcap::PcapReader reader(pcap_data);

    CHECK(reader.is_valid());
    CHECK(reader.is_nanosecond());
    CHECK(reader.link_type() == mog::pcap::kLinkTypeEthernet);

    std::size_t packets_seen = 0;
    MessageCollector collector;

    const std::size_t count = reader.for_each_packet([&](const mog::pcap::PacketView& pkt) {
        ++packets_seen;
        CHECK(pkt.src_port == 12345);
        CHECK(pkt.dst_port == 54321);
        CHECK(pkt.protocol == 17);
        CHECK(pkt.ts_ns == 1600000000000500000ULL);

        const auto res = mog::parse_itch(pkt.payload.data(), pkt.payload.size(), collector);
        CHECK(res.has_value());
    });

    CHECK(count == 1);
    CHECK(packets_seen == 1);
    CHECK(collector.count == 1);
    CHECK(collector.last_ref == 1234567ULL);
}

} // namespace

int main() {
    test_pcap_decoding();
    return 0;
}
