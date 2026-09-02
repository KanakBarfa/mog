// Arrow C Data Interface E2E integration test: verifies schema descriptors,
// memory buffer layout, boolean bitmask packing, and recursive release semantics.
#include <mog/Arrow.hpp>
#include <mog/Simulate.hpp>
#include <mog/Strategy.hpp>
#include <mog/Trades.hpp>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

void test_trades_export() {
    std::vector<mog::trades::Print> prints;
    for (std::uint64_t i = 1; i <= 100; ++i) {
        prints.push_back(mog::trades::Print{
            .match_number = i * 1000,
            .locate = static_cast<std::uint32_t>(i % 5),
            .ts_ns = 1'000'000'000ULL + i * 100,
            .price_ticks = static_cast<std::int64_t>(10000 + i * 25),
            .shares = static_cast<std::uint32_t>(i * 10),
            .printable = (i % 2 == 0),
            .from_execute_with_price = (i % 3 == 0),
        });
    }

    ArrowSchema schema{};
    ArrowArray array{};
    mog::arrow::export_trades(prints, &array, &schema);

    // Verify Schema Layout
    CHECK(schema.release != nullptr);
    CHECK(std::strcmp(schema.format, "+s") == 0);
    CHECK(schema.n_children == 7);
    CHECK(std::strcmp(schema.children[0]->format, "L") == 0);
    CHECK(std::strcmp(schema.children[0]->name, "match_number") == 0);
    CHECK(std::strcmp(schema.children[1]->format, "I") == 0);
    CHECK(std::strcmp(schema.children[1]->name, "locate") == 0);
    CHECK(std::strcmp(schema.children[2]->format, "L") == 0);
    CHECK(std::strcmp(schema.children[2]->name, "ts_ns") == 0);
    CHECK(std::strcmp(schema.children[3]->format, "l") == 0);
    CHECK(std::strcmp(schema.children[3]->name, "price_ticks") == 0);
    CHECK(std::strcmp(schema.children[4]->format, "I") == 0);
    CHECK(std::strcmp(schema.children[4]->name, "shares") == 0);
    CHECK(std::strcmp(schema.children[5]->format, "b") == 0);
    CHECK(std::strcmp(schema.children[5]->name, "printable") == 0);
    CHECK(std::strcmp(schema.children[6]->format, "b") == 0);
    CHECK(std::strcmp(schema.children[6]->name, "from_execute_with_price") == 0);

    // Verify Array Data
    CHECK(array.release != nullptr);
    CHECK(array.length == 100);
    CHECK(array.null_count == 0);
    CHECK(array.n_children == 7);

    const auto* match_buf = static_cast<const std::uint64_t*>(array.children[0]->buffers[1]);
    const auto* locate_buf = static_cast<const std::uint32_t*>(array.children[1]->buffers[1]);
    const auto* ts_buf = static_cast<const std::uint64_t*>(array.children[2]->buffers[1]);
    const auto* price_buf = static_cast<const std::int64_t*>(array.children[3]->buffers[1]);
    const auto* shares_buf = static_cast<const std::uint32_t*>(array.children[4]->buffers[1]);
    const auto* print_bits = static_cast<const std::uint8_t*>(array.children[5]->buffers[1]);
    const auto* with_px_bits = static_cast<const std::uint8_t*>(array.children[6]->buffers[1]);

    for (std::size_t i = 0; i < 100; ++i) {
        CHECK(match_buf[i] == prints[i].match_number);
        CHECK(locate_buf[i] == prints[i].locate);
        CHECK(ts_buf[i] == prints[i].ts_ns);
        CHECK(price_buf[i] == prints[i].price_ticks);
        CHECK(shares_buf[i] == prints[i].shares);
        const bool print_val = (print_bits[i / 8] & (1u << (i % 8))) != 0;
        CHECK(print_val == prints[i].printable);
        const bool with_px_val = (with_px_bits[i / 8] & (1u << (i % 8))) != 0;
        CHECK(with_px_val == prints[i].from_execute_with_price);
    }

    // Release Resources
    schema.release(&schema);
    CHECK(schema.release == nullptr);
    array.release(&array);
    CHECK(array.release == nullptr);
}

void test_fill_reports_and_decisions() {
    std::vector<mog::SimFillReport> reports;
    reports.push_back(mog::SimFillReport{
        .visible_ts = 500,
        .ref = 101,
        .price_ticks = 4000,
        .qty = 50,
        .seq = 1,
        .side = 'B',
        .fee = -2,
    });

    ArrowSchema schema{};
    ArrowArray array{};
    mog::arrow::export_fill_reports(reports, &array, &schema);
    CHECK(schema.n_children == 7);
    CHECK(array.length == 1);
    const auto* qty_buf = static_cast<const std::uint32_t*>(array.children[3]->buffers[1]);
    CHECK(qty_buf[0] == 50);
    schema.release(&schema);
    array.release(&array);

    std::vector<mog::SimDecision> decisions;
    decisions.push_back(mog::SimDecision{
        .kind = mog::SimDecision::Kind::filled,
        .filled_qty = 100,
        .cancelled_qty = 0,
        .filled_notional = 400000,
        .visible_ts = 800,
        .ref = 202,
        .side = 'S',
        .seq = 2,
        .fee = 10,
    });

    ArrowSchema dec_schema{};
    ArrowArray dec_array{};
    mog::arrow::export_decisions(decisions, &dec_array, &dec_schema);
    CHECK(dec_schema.n_children == 9);
    CHECK(dec_array.length == 1);
    const auto* notional_buf = static_cast<const std::uint64_t*>(dec_array.children[3]->buffers[1]);
    CHECK(notional_buf[0] == 400000);
    dec_schema.release(&dec_schema);
    dec_array.release(&dec_array);
}

void test_empty_export() {
    ArrowSchema schema{};
    ArrowArray array{};
    mog::arrow::export_trades({}, &array, &schema);
    CHECK(array.length == 0);
    CHECK(schema.n_children == 7);
    schema.release(&schema);
    array.release(&array);
}

void test_reflected_records_export() {
    std::vector<mog::BookUpdate> updates;
    updates.push_back(mog::BookUpdate{
        .ts = 1'000'000,
        .bid_px = 2000,
        .bid_qty = 100,
        .ask_px = 2005,
        .ask_qty = 150,
    });

    ArrowSchema schema{};
    ArrowArray array{};
    mog::arrow::export_reflected_records(std::span<const mog::BookUpdate>{updates}, &array,
                                         &schema);
    CHECK(schema.n_children == 5);
    CHECK(array.length == 1);
    CHECK(std::strcmp(schema.children[0]->name, "ts") == 0);
    CHECK(std::strcmp(schema.children[1]->name, "bid_px") == 0);
    CHECK(std::strcmp(schema.children[2]->name, "bid_qty") == 0);
    CHECK(std::strcmp(schema.children[3]->name, "ask_px") == 0);
    CHECK(std::strcmp(schema.children[4]->name, "ask_qty") == 0);

    const auto* bid_px_buf = static_cast<const std::int64_t*>(array.children[1]->buffers[1]);
    CHECK(bid_px_buf[0] == 2000);
    schema.release(&schema);
    array.release(&array);
}

} // namespace

int main() {
    test_trades_export();
    test_fill_reports_and_decisions();
    test_empty_export();
    test_reflected_records_export();
    std::printf("arrow_e2e passed\n");
    return 0;
}
