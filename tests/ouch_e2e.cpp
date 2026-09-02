// OUCH 5.0 binary protocol E2E integration test: verifies wire layout,
// big-endian codecs, gateway event routing, and outbound frame generation.
#include <mog/Contracts.hpp>
#include <mog/OUCH.hpp>
#include <mog/Simulate.hpp>

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

void test_ouch_wire_sizes_and_framing() {
    CHECK(sizeof(mog::ouch::EnterOrderWire) == 49);
    CHECK(sizeof(mog::ouch::ReplaceOrderWire) == 47);
    CHECK(sizeof(mog::ouch::CancelOrderWire) == 19);
    CHECK(sizeof(mog::ouch::ModifyOrderWire) == 20);
    CHECK(sizeof(mog::ouch::SystemEventWire) == 10);
    CHECK(sizeof(mog::ouch::OrderAcceptedWire) == 66);
    CHECK(sizeof(mog::ouch::OrderExecutedWire) == 40);
    CHECK(sizeof(mog::ouch::OrderCancelledWire) == 28);
    CHECK(sizeof(mog::ouch::OrderReplacedWire) == 80);
    CHECK(sizeof(mog::ouch::OrderRejectedWire) == 24);

    CHECK(mog::ouch::inbound_frame_length('O') == 49);
    CHECK(mog::ouch::inbound_frame_length('U') == 47);
    CHECK(mog::ouch::inbound_frame_length('X') == 19);
    CHECK(mog::ouch::outbound_frame_length('A') == 66);
    CHECK(mog::ouch::outbound_frame_length('E') == 40);
    CHECK(mog::ouch::outbound_frame_length('C') == 28);
    CHECK(mog::ouch::outbound_frame_length('J') == 24);
}

void test_ouch_gateway_lifecycle() {
    mog::SimConfig cfg{};
    cfg.book = {1u << 16, {0, 10000, 32}};
    cfg.event_capacity = 1024;
    cfg.external_ref_limit = 1000;
    cfg.seed = 42;

    mog::ExecutionSimulator sim(cfg);
    mog::ouch::OuchGateway gateway(sim, {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '});

    // Seed liquidity
    static_cast<void>(
        sim.seed_external(mog::OrderId{1}, mog::Side::buy, mog::Qty{100}, mog::Price{2000}));
    static_cast<void>(
        sim.seed_external(mog::OrderId{2}, mog::Side::sell, mog::Qty{100}, mog::Price{2010}));

    // Enter order via OUCH wire frame
    mog::ouch::EnterOrderWire enter{};
    mog::ouch::EnterOrderMsg msg{};
    msg.token = mog::ouch::token_from_id(7001);
    msg.side = mog::Side::buy;
    msg.shares = 50;
    msg.stock = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    msg.price_ticks = 2005;
    msg.tif = 0; // Day
    msg.display = 'Y';
    mog::ouch::encode_enter_order(msg, enter);

    const auto arrival_seq = gateway.submit_enter(enter, 100);
    CHECK(arrival_seq > 0);

    // Advance time and drain decisions
    sim.advance_time(500);
    sim.drain();

    std::vector<std::vector<unsigned char>> outbound;
    gateway.drain_outbound(outbound);

    // Expect OrderAccepted frame
    CHECK(!outbound.empty());
    const auto* accepted =
        reinterpret_cast<const mog::ouch::OrderAcceptedWire*>(outbound[0].data());
    CHECK(accepted->type == 'A');
    CHECK(accepted->buy_sell == 'B');
    CHECK(mog::wire::load_be32(reinterpret_cast<const unsigned char*>(&accepted->shares)) == 50);

    // Apply external match against resting order
    sim.apply_external(mog::Side::buy, mog::Price{2005}, 50);
    sim.advance_time(100);
    sim.drain();

    outbound.clear();
    gateway.drain_outbound(outbound);
    CHECK(!outbound.empty());
    const auto* executed =
        reinterpret_cast<const mog::ouch::OrderExecutedWire*>(outbound[0].data());
    CHECK(executed->type == 'E');
    CHECK(mog::wire::load_be32(
              reinterpret_cast<const unsigned char*>(&executed->executed_shares)) == 50);

    CHECK(gateway.trace_hash() != 0);

    // Test submit_replace -> OrderReplacedWire ('U')
    mog::ouch::EnterOrderWire enter2{};
    mog::ouch::EnterOrderMsg msg2{};
    msg2.token = mog::ouch::token_from_id(7002);
    msg2.side = mog::Side::buy;
    msg2.shares = 100;
    msg2.stock = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    msg2.price_ticks = 1990;
    msg2.tif = 0;
    msg2.display = 'Y';
    mog::ouch::encode_enter_order(msg2, enter2);
    static_cast<void>(gateway.submit_enter(enter2, 200));
    sim.advance_time(100);
    sim.drain();
    outbound.clear();
    gateway.drain_outbound(outbound);

    mog::ouch::ReplaceOrderWire rep_wire{};
    mog::ouch::ReplaceOrderMsg rep_msg{};
    rep_msg.existing_token = mog::ouch::token_from_id(7002);
    rep_msg.replacement_token = mog::ouch::token_from_id(7003);
    rep_msg.shares = 120;
    rep_msg.price_ticks = 1995;
    rep_msg.tif = 0;
    rep_msg.display = 'Y';
    mog::ouch::encode_replace_order(rep_msg, rep_wire);

    const bool rep_ok = gateway.submit_replace(rep_wire, 300);
    CHECK(rep_ok);

    outbound.clear();
    gateway.drain_outbound(outbound);
    CHECK(!outbound.empty());
    const auto* replaced =
        reinterpret_cast<const mog::ouch::OrderReplacedWire*>(outbound[0].data());
    CHECK(replaced->type == 'U');
    CHECK(mog::wire::load_be32(reinterpret_cast<const unsigned char*>(&replaced->shares)) == 120);
    CHECK(mog::wire::load_be32(reinterpret_cast<const unsigned char*>(&replaced->price)) == 1995);

    // Test submit_cancel
    mog::ouch::CancelOrderWire cancel_wire{};
    cancel_wire.type = 'X';
    std::memcpy(cancel_wire.order_token, rep_msg.replacement_token.data(), 14);
    mog::wire::store_be32(reinterpret_cast<unsigned char*>(&cancel_wire.shares), 0);
    const bool cancel_ok = gateway.submit_cancel(cancel_wire);
    CHECK(cancel_ok);

    // Test STP AIQ Cancelled ('D')
    sim.set_stp_mode(mog::StpMode::cancel_newest);
    // Seed resting strategy sell order inside the spread (best ask is 2010, so place at 2008)
    mog::ouch::EnterOrderWire enter_sell{};
    mog::ouch::EnterOrderMsg sell_msg{};
    sell_msg.token = mog::ouch::token_from_id(7004);
    sell_msg.side = mog::Side::sell;
    sell_msg.shares = 50;
    sell_msg.stock = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    sell_msg.price_ticks = 2008;
    sell_msg.tif = 0;
    sell_msg.display = 'Y';
    mog::ouch::encode_enter_order(sell_msg, enter_sell);
    static_cast<void>(gateway.submit_enter(enter_sell, 400));
    sim.advance_time(100);
    sim.drain();
    outbound.clear();
    gateway.drain_outbound(outbound);

    // Incoming strategy buy order crossing the resting strategy sell order
    mog::ouch::EnterOrderWire enter_buy_cross{};
    mog::ouch::EnterOrderMsg buy_cross_msg{};
    buy_cross_msg.token = mog::ouch::token_from_id(7005);
    buy_cross_msg.side = mog::Side::buy;
    buy_cross_msg.shares = 50;
    buy_cross_msg.stock = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    buy_cross_msg.price_ticks = 2008;
    buy_cross_msg.tif = 0;
    buy_cross_msg.display = 'Y';
    mog::ouch::encode_enter_order(buy_cross_msg, enter_buy_cross);
    static_cast<void>(gateway.submit_enter(enter_buy_cross, 500));
    sim.advance_time(100);
    sim.drain();

    outbound.clear();
    gateway.drain_outbound(outbound);
    CHECK(!outbound.empty());
    const auto* aiq = reinterpret_cast<const mog::ouch::AIQCancelledWire*>(outbound[0].data());
    CHECK(aiq->type == 'D');
    CHECK(aiq->reason == 'Q');
}

} // namespace

int main() {
    test_ouch_wire_sizes_and_framing();
    test_ouch_gateway_lifecycle();
    std::printf("ouch_e2e passed\n");
    return 0;
}
