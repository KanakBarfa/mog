// Integration smoke: contract-abort end to end, CLI byte determinism, ITCH codec round
// trips against wire offsets, allocation guard arming. Runs its own binary as children.

#include <mog/AllocGuard.hpp>
#include <mog/Build.hpp>
#include <mog/Contracts.hpp>
#include <mog/Types.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

int fail(const char* what) noexcept {
    std::fprintf(stderr, "smoke FAIL: %s\n", what);
    return 1;
}

struct Captured {
    std::string out;
    std::string err;
    int signaled_sig = 0;
    int exit_code = -1;
};

Captured spawn_self(const char* self_path, const char* arg) noexcept {
    Captured cap;
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        cap.err = "pipe failed";
        return cap;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        cap.err = "fork failed";
        return cap;
    }
    if (pid == 0) {
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        char* argv_child[] = {const_cast<char*>(self_path), const_cast<char*>(arg), nullptr};
        execv(self_path, argv_child);
        _exit(127);
    }
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    auto drain = [&](int fd, std::string& into) {
        char buf[4096];
        for (;;) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0)
                continue;
            if (n == 0)
                break;
            into.append(buf, static_cast<std::size_t>(n));
        }
        ::close(fd);
    };
    drain(out_pipe[0], cap.out);
    drain(err_pipe[0], cap.err);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status))
        cap.signaled_sig = WTERMSIG(status);
    else if (WIFEXITED(status))
        cap.exit_code = WEXITSTATUS(status);
    return cap;
}

[[nodiscard]] mog::AddOrderMsg sample_add_order() noexcept {
    mog::AddOrderMsg m{};
    m.header.locate = mog::Locate{7};
    m.header.tracking = mog::Tracking{0};
    m.header.ts_ns = mog::TimestampNs{34'200'000'000'000ULL};
    m.order_ref = mog::OrderId{123'456'789ULL};
    m.side = mog::Side::buy;
    m.shares = mog::Qty{500};
    m.stock = mog::symbol_from("AAPL");
    m.price = mog::Price{1'892'500};
    return m;
}

int check_contract_abort_e2e(const char* self_path) {
    const Captured cap = spawn_self(self_path, "--smoke-child-violate");
    if (cap.signaled_sig != SIGABRT)
        return fail("violation must abort with SIGABRT");
    if (!cap.err.starts_with("MOG CONTRACT VIOLATION"))
        return fail("violation report missing");
    return 0;
}

int check_cli_determinism(const char* self_path) {
    const Captured a = spawn_self(self_path, "--smoke-child-info");
    const Captured b = spawn_self(self_path, "--smoke-child-info");
    if (a.out.empty())
        return fail("determinism probe produced no output");
    if (a.out != b.out || a.err != b.err)
        return fail("child output not deterministic");
    return 0;
}

int check_codecs() {
    using namespace mog;

    // Add Order
    const AddOrderMsg add = sample_add_order();
    unsigned char buf[wire::kAddOrderSize];
    encode_add_order(add, buf);
    if (buf[0] != 'A' || buf[19] != 'B')
        return fail("add order wire header bytes");
    if (wire::load_be32(buf + 32) != 1'892'500U)
        return fail("price offset");
    if (wire::load_be64(buf + 11) != 123'456'789ULL)
        return fail("order ref offset");
    auto back = decode_add_order(buf, sizeof(buf));
    if (!back || !(*back == add))
        return fail("add order round trip");
    auto dispatched = decode_message(buf, sizeof(buf));
    if (!dispatched || dispatched->type != 'A')
        return fail("dispatch add order");

    // Truncation and bad type
    if (decode_add_order(buf, wire::kAddOrderSize - 1).error() != DecodeError::truncated)
        return fail("truncation must be reported");
    unsigned char zbuf[wire::kHeaderSize] = {'Z'};
    if (decode_message(zbuf, sizeof(zbuf)).error() != DecodeError::unknown_type)
        return fail("unknown type must be reported");

    // Side validation
    unsigned char sbuf[wire::kAddOrderSize];
    __builtin_memcpy(sbuf, buf, sizeof(buf));
    sbuf[19] = 'Q';
    if (decode_add_order(sbuf, sizeof(sbuf)).error() != DecodeError::invalid_side)
        return fail("invalid side must be reported");

    // F attribution
    AddOrderAttributionMsg fa{};
    fa = AddOrderAttributionMsg{
        .header = add.header,
        .order_ref = add.order_ref,
        .side = add.side,
        .shares = add.shares,
        .stock = add.stock,
        .price = add.price,
        .attribution = {'F', 'I', 'R', 'M'},
    };
    unsigned char fbuf[wire::kAddOrderAttributionSize];
    encode_add_order_attribution(fa, fbuf);
    if (fbuf[0] != 'F')
        return fail("attribution wire type");
    auto faback = decode_add_order_attribution(fbuf, sizeof(fbuf));
    if (!faback || !(*faback == fa))
        return fail("attribution round trip");

    // E executed
    const OrderExecutedMsg ex{
        .header = add.header,
        .order_ref = add.order_ref,
        .executed_shares = Qty{300},
        .match_number = MatchNumber{42},
    };
    unsigned char ebuf[wire::kOrderExecutedSize];
    encode_order_executed(ex, ebuf);
    if (ebuf[0] != 'E')
        return fail("executed wire type");
    auto exback = decode_order_executed(ebuf, sizeof(ebuf));
    if (!exback || !(*exback == ex))
        return fail("executed round trip");

    // C executed with price
    const OrderExecutedWithPriceMsg exc{
        .header = add.header,
        .order_ref = add.order_ref,
        .executed_shares = Qty{200},
        .match_number = MatchNumber{43},
        .printable = true,
        .execution_price = Price{1'893'000},
    };
    unsigned char cbuf[wire::kOrderExecutedWithPriceSize];
    encode_order_executed_with_price(exc, cbuf);
    auto cback = decode_order_executed_with_price(cbuf, sizeof(cbuf));
    if (!cback || !(*cback == exc))
        return fail("executed-with-price round trip");

    // X cancel
    const OrderCancelMsg can{
        .header = add.header,
        .order_ref = add.order_ref,
        .cancelled_shares = Qty{100},
    };
    unsigned char xbuf[wire::kOrderCancelSize];
    encode_order_cancel(can, xbuf);
    auto xback = decode_order_cancel(xbuf, sizeof(xbuf));
    if (!xback || !(*xback == can))
        return fail("cancel round trip");

    // D delete
    const OrderDeleteMsg del{
        .header = add.header,
        .order_ref = add.order_ref,
    };
    unsigned char dbuf[wire::kOrderDeleteSize];
    encode_order_delete(del, dbuf);
    if (dbuf[0] != 'D')
        return fail("delete wire type");
    auto dback = decode_order_delete(dbuf, sizeof(dbuf));
    if (!dback || !(*dback == del))
        return fail("delete round trip");

    // U replace
    const OrderReplaceMsg rep{
        .header = add.header,
        .original_order_ref = add.order_ref,
        .new_order_ref = OrderId{987'654'321ULL},
        .shares = Qty{700},
        .price = Price{1'892'000},
    };
    unsigned char ubuf[wire::kOrderReplaceSize];
    encode_order_replace(rep, ubuf);
    auto uback = decode_order_replace(ubuf, sizeof(ubuf));
    if (!uback || !(*uback == rep))
        return fail("replace round trip");

    // Symbol padding semantics
    if (symbol_view(symbol_from("MSFT")) != "MSFT")
        return fail("symbol trim");
    return 0;
}

int check_alloc_guard() {
    using namespace mog;
    if (alloc::is_armed())
        return fail("guard must start disarmed");
    alloc::arm();
    std::int64_t acc = 0;
    Price p{100};
    for (int i = 0; i < 10'000; ++i)
        p = p.raw_add(Price{1});
    acc += p.ticks;
    alloc::disarm();
    if (acc <= 0)
        return fail("guarded math ran");
    int* probe = new int(7);
    const bool ok = *probe == 7;
    delete probe;
    if (!ok)
        return fail("post-disarm allocation");
    return 0;
}

int run_tests(const char* self_path) {
    if (int r = check_contract_abort_e2e(self_path); r != 0)
        return r;
    if (int r = check_cli_determinism(self_path); r != 0)
        return r;
    if (int r = check_codecs(); r != 0)
        return r;
    if (int r = check_alloc_guard(); r != 0)
        return r;
    std::printf("smoke OK\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--smoke-child-violate") == 0) {
        mog::contracts::set_mode(mog::contracts::Mode::enforce);
        MOG_CONTRACT_ASSERT(false);
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--smoke-child-info") == 0) {
        std::fputs("mog-smoke determinism probe\n", stdout);
        return 0;
    }
    return run_tests(argv[0]);
}
