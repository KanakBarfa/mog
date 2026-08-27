// Deterministic ITCH corpus generation shared by tests, benchmarks, and tooling.
#pragma once

#include <mog/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mog::testing {

[[nodiscard]] inline std::uint64_t next_splitmix(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

struct CorpusSpec {
    std::uint64_t seed = 0x5EEDULL;
    std::size_t count = 1024;
    // Price band for generated orders: prices are drawn uniformly from
    // [price_center, price_center + price_span). The default spans the full
    // legacy range; a narrow band produces realistic touch-clustered books
    // that fit modest ladder page pools.
    std::int64_t price_center = 0;
    std::int64_t price_span = 10'000'000;
};

constexpr char kSymbols[][8 + 1] = {
    "SPY", "AAPL", "MSFT", "NVDA", "TSLA", "AMZN", "GOOG", "META",
};

[[nodiscard]] inline Symbol corpus_symbol(std::uint64_t pick) noexcept {
    return symbol_from(kSymbols[pick % 8]);
}

[[nodiscard]] inline std::int64_t draw_price_in_band(std::uint64_t& rng,
                                                     const CorpusSpec& spec) noexcept {
    return spec.price_center +
           static_cast<std::int64_t>(next_splitmix(rng) %
                                     static_cast<std::uint64_t>(spec.price_span));
}

[[nodiscard]] inline Message make_message(std::uint64_t& rng, std::size_t index,
                                          std::uint64_t& next_order_ref,
                                          const CorpusSpec& spec = CorpusSpec{}) noexcept {
    static constexpr char kTypeOrder[7] = {'A', 'F', 'E', 'C', 'X', 'D', 'U'};
    const char type = kTypeOrder[index % 7];
    const std::uint64_t ts = 34'200'000'000'000ULL + index * 1'000ULL + (next_splitmix(rng) % 900);
    Header hdr{};
    hdr.locate = Locate{static_cast<std::uint16_t>(next_splitmix(rng) & 0x1FFF)};
    hdr.tracking = Tracking{0};
    hdr.ts_ns = TimestampNs{ts};
    Message m{};
    m.type = type;
    switch (type) {
    case 'A':
    case 'F': {
        const Side side = (next_splitmix(rng) & 1) != 0 ? Side::buy : Side::sell;
        const Qty shares{static_cast<std::int64_t>(next_splitmix(rng) % 1'000'000 + 1)};
        const Price price{draw_price_in_band(rng, spec)};
        if (type == 'A') {
            m.add_order = AddOrderMsg{.header = hdr,
                                      .order_ref = OrderId{next_order_ref},
                                      .side = side,
                                      .shares = shares,
                                      .stock = corpus_symbol(next_splitmix(rng)),
                                      .price = price};
        } else {
            m.add_order_attribution =
                AddOrderAttributionMsg{.header = hdr,
                                       .order_ref = OrderId{next_order_ref},
                                       .side = side,
                                       .shares = shares,
                                       .stock = corpus_symbol(next_splitmix(rng)),
                                       .price = price,
                                       .attribution = {'F', 'I', 'R', 'M'}};
        }
        ++next_order_ref;
        break;
    }
    case 'E': {
        m.order_executed = OrderExecutedMsg{
            .header = hdr,
            .order_ref = OrderId{next_splitmix(rng)},
            .executed_shares = Qty{static_cast<std::int64_t>(next_splitmix(rng) % 500'000 + 1)},
            .match_number = MatchNumber{next_order_ref}};
        ++next_order_ref;
        break;
    }
    case 'C': {
        m.order_executed_with_price = OrderExecutedWithPriceMsg{
            .header = hdr,
            .order_ref = OrderId{next_splitmix(rng)},
            .executed_shares = Qty{static_cast<std::int64_t>(next_splitmix(rng) % 500'000 + 1)},
            .match_number = MatchNumber{next_order_ref},
            .printable = (next_splitmix(rng) & 1) != 0,
            .execution_price = Price{static_cast<std::int64_t>(next_splitmix(rng) % 10'000'000)}};
        ++next_order_ref;
        break;
    }
    case 'X': {
        m.order_cancel = OrderCancelMsg{
            .header = hdr,
            .order_ref = OrderId{next_splitmix(rng)},
            .cancelled_shares = Qty{static_cast<std::int64_t>(next_splitmix(rng) % 400'000 + 1)}};
        break;
    }
    case 'D': {
        m.order_delete = OrderDeleteMsg{.header = hdr, .order_ref = OrderId{next_splitmix(rng)}};
        break;
    }
    case 'U': {
        m.order_replace = OrderReplaceMsg{
            .header = hdr,
            .original_order_ref = OrderId{next_splitmix(rng)},
            .new_order_ref = OrderId{next_order_ref},
            .shares = Qty{static_cast<std::int64_t>(next_splitmix(rng) % 900'000 + 1)},
            .price = Price{static_cast<std::int64_t>(next_splitmix(rng) % 10'000'000)}};
        ++next_order_ref;
        break;
    }
    default:
        break;
    }
    return m;
}

inline void append_encoded(std::vector<unsigned char>& out, const Message& m) noexcept {
    unsigned char scratch[wire::kMaxMessageSize];
    std::size_t n = 0;
    switch (m.type) {
    case 'A':
        encode_add_order(m.add_order, scratch);
        n = wire::kAddOrderSize;
        break;
    case 'F':
        encode_add_order_attribution(m.add_order_attribution, scratch);
        n = wire::kAddOrderAttributionSize;
        break;
    case 'E':
        encode_order_executed(m.order_executed, scratch);
        n = wire::kOrderExecutedSize;
        break;
    case 'C':
        encode_order_executed_with_price(m.order_executed_with_price, scratch);
        n = wire::kOrderExecutedWithPriceSize;
        break;
    case 'X':
        encode_order_cancel(m.order_cancel, scratch);
        n = wire::kOrderCancelSize;
        break;
    case 'D':
        encode_order_delete(m.order_delete, scratch);
        n = wire::kOrderDeleteSize;
        break;
    case 'U':
        encode_order_replace(m.order_replace, scratch);
        n = wire::kOrderReplaceSize;
        break;
    default:
        return;
    }
    out.insert(out.end(), scratch, scratch + n);
}

[[nodiscard]] inline std::vector<Message> make_corpus(const CorpusSpec& spec) {
    std::vector<Message> messages;
    messages.reserve(spec.count);
    std::uint64_t rng = spec.seed;
    std::uint64_t order_ref = 1;
    for (std::size_t i = 0; i < spec.count; ++i) {
        messages.push_back(make_message(rng, i, order_ref, spec));
    }
    return messages;
}

[[nodiscard]] inline std::vector<unsigned char>
encode_corpus(const std::vector<Message>& messages) {
    std::vector<unsigned char> bytes;
    bytes.reserve(messages.size() * 32);
    for (const Message& m : messages)
        append_encoded(bytes, m);
    return bytes;
}

} // namespace mog::testing
