# Parser design note (M1)

Scope: ITCH 5.0 subset A/F/E/C/X/D/U, stream framing through kernel selection.
Reference codecs live in `include/mog/Types.hpp`; the streaming layer in
`include/mog/ITCHParser.hpp`.

## Wire layout

Shared 11-byte header: type (1) at 0, stock locate (2, BE) at 1, tracking
number (2, BE) at 3, timestamp (6, BE) at 5. Timestamps are nanoseconds since
midnight; the 48-bit width is handled by `wire::load_be48`/`store_be48`
(u16 + u32 big-endian loads, no overread).

| Type | Message                  | Size | Payload offsets after header |
|---|---|---|---|
| A    | Add Order                | 36   | ref@11, side@19, shares@20, stock@24, price@32 |
| F    | Add Order w/ Attribution | 40   | as A plus attribution@36 |
| E    | Order Executed           | 31   | ref@11, shares@19, match@23 |
| C    | Executed With Price      | 36   | ref@11, shares@19, match@23, printable@31, px@32 |
| X    | Order Cancel             | 23   | ref@11, shares@19 |
| D    | Order Delete             | 19   | ref@11 |
| U    | Order Replace            | 35   | orig@11, new@19, shares@27, price@31 |

Every offset is pinned by `static_assert(offsetof(...))` audits in
`mog/wire`; drift fails the build.

## Layers

1. **Reference codecs** (`Types.hpp`): memcpy packed wire struct to stack,
   per-field byteswap into decoded types. Acceptance: truncated below message
   size; unknown type; side byte not B/S.
2. **Fast path** (`ITCHParser::detail::parse_one<Isa>`): loads fields directly
   from the stream pointer, no staging copy. Symbol moves as one unaligned
   8-byte word (`swar::load64/store64`). Acceptance set is byte-identical to
   the reference by construction and by test.
3. **Framing**: constexpr 256-entry length LUT indexed by type byte; unknown
   type or short tail is a hard error carrying the stream offset
   (`ParseError{code, offset}`).
4. **Dispatch**: switch on type char (the compiler emits a jump table). The
   planned P2996 expansion-statement table generates exactly this shape; when
   the lab toolchain lands it must produce a bit-identical dispatch, verified
   by the same differential corpus.
5. **ISA selection**: `detect_isa()` via CPUID picks sse4_baseline / avx2 /
   avx512 at first use; `parse_itch_as` forces a variant for tests and
   benches. Kernels are textually shared today; variants diverge only when an
   ISA-specific body earns its place under doctrine #4.

## Differential testing

`tests/parser_diff.cpp` walks a seeded corpus (splitmix64, all seven types,
boundary-magnitude fields) and requires:

- reference hash sequence == every ISA kernel's hash sequence,
- truncation sweep at every byte cut of every message type: accept/reject
  agreement with the reference codec,
- unknown-type and invalid-side parity on crafted streams.

Gate run for this milestone: **100,000,000 messages x 3 kernels, divergence 0**
(6m22s wall on the bench machine, single thread, contracts enforced).

## Instruction budget

Machine-independent metric from `bench/parser_ir.cpp` under callgrind
(collection toggled around each full-corpus pass):

| Component | Ir/msg |
|---|---|
| Framing + active-kernel decode into Message | 79.6 |
| Trace-hash sink folding every field | 39.7 |
| Total measured loop | 119.3 |

The decode-only figure is the engine cost; the sink is test instrumentation
standing in for downstream consumers. In the replay pipeline that slot is
filled by book application instead, so parser headroom beyond ~80 Ir/msg was
deliberately not pursued: absolute savings there are smaller than remaining
book-path costs, and both throughput gates are already met.

## Performance (i5-7500T @ 3.3GHz boost, GCC 15.2, -O3 -march=native)

| Benchmark | Result |
|---|---|
| Full stream parse incl. trace-hash sink | 17.7 ns/msg = **59.2M msg/s** |
| Baseline-kernel stream parse | 17.6 ns/msg = 59.5M msg/s |
| Single Add Order decode (baseline kernel) | 2.56 ns = 390M msg/s |

Exit criterion >50M msg/s median is met including sink work; the kernel alone
runs an order of magnitude above it.

## Hand-written asm verdict: REJECTED

Attempted twin: full Add Order extraction in inline asm (8 movbe/movq loads,
early-clobber-constrained outputs), verified field-equal against the C++
kernel before timing (`SkipWithError` on any divergence).

| Kernel | ns/msg |
|---|---|
| asm twin | 2.88 |
| compiler output (-march=native) | 2.56 |

The compiler already emits movbe for our bswap loads and schedules better than
hand allocation; the twin loses ~13%, far from the >=15% win doctrine #4
requires. Rejected; the benchmark remains as the harness any future attempt
must beat. Two implementation traps are worth recording for the next attempt:
outputs sharing the input register without early-clobber marks (miscompile +
SIGSEGV), and a non-swapped 16-bit half of the 48-bit timestamp (silent wrong
values caught by the twin guard).

## Deviations from plan

- Reflection-generated codec leg unavailable: P2996 needs a lab toolchain not
  installed here. Hand-written twins carry the semantics; generated twins must
  pass this same differential suite before replacing them.
- `std::simd` SWAR baseline replaced by word-at-a-time primitives
  (`swar::load64/store64`); libstdc++ 15 ships no `<simd>`. Same replacement
  policy applies once available.

## Reproduction

```sh
cmake -B build/frontier -G Ninja -DMOG_PROFILE=frontier
cmake --build build/frontier
ctest --test-dir build/frontier
MOG_DIFF_N=100000000 ./build/frontier/tests/mog-diff
./build/frontier/bench/mog-bench --benchmark_min_time=1s
```
