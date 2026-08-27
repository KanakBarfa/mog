# O1 executed: reconstruction vs official Nasdaq Last Sale 4.0 tape

**First free paired-sample execution of the official-file diff.** Source:
emi.nasdaq.com's designed ITCH/NLS pair - `itch50_05_15.gz` (ITCH 5.0,
2.45 GB raw prefix = first 5h36m of the session, 04:00-09:36) against
`nls40_05_15.gz` (official Nasdaq Last Sale v4.0, binary Trade Report
format per the public spec). Both files open at the identical nanosecond
(14,400,003,048,089) - same trading day proven to the digit.

## Headline numbers

| Metric | Value |
|---|---|
| Our prints (E/C book executions) | 1,151,518 |
| Unpriced E/C in reconstruction | **0 (0.00%)** |
| Off-book trade messages (P non-cross + Q cross) | 660,870 |
| Official Q-center tape rows in window | 2,228,085 |
| Exact record containment (E/C alone vs tape) | **99.94%** (690 misses) |
| Exact record containment (E/C + P/Q vs tape) | **99.26%** (13,345 misses) |
| Per-(symbol,price) volume buckets exactly reconciled | **94.54%** of 226,150 |
| Buckets within 2% | 94.64% |

## Why the residuals are not divergence

1. **Report-time aggregation.** The official tape bundles consecutive
   same-price executions into single larger reports stamped microseconds
   after our fills (verified directly: RKLZ $2.54 - 32 of our prints
   totaling 13,500 shares sit next to one 76,086-share tape report;
   same pattern for TELO, QYLD, DFLIW). Record-level equality is
   therefore structurally bounded; volume reconciliation is the honest
   metric.
2. **Timestamp drift.** ~253 of the E/C misses match the tape on
   (symbol, price, shares) but not to the exact nanosecond.
3. **Tape-only liquidity.** The tape carries Nasdaq-book volume with no
   ITCH counterpart at all (ref-only records are 98.3% round-lot+ by
   volume): executions disseminated only through last-sale reporting -
   consistent with O2's finding that every message we DO receive prices
   perfectly.

## Reproduction

```
g++-15 -std=c++26 -O2 -I include -I build/portable/generated \
    -DMOG_PROFILE_PORTABLE=1 -DMOG_CXX_STANDARD=26 tools/unpriced_classify.cpp
./a.out <itch-prefix> stats.csv prints.csv      # ours side
python3 tools/nls_diff.py <nls-binary> prints.csv --size-divisor 1000000
```

Size(6): this NLS v4.0 tape carries implied-decimal sizes; divisor 1e6
matches our integer shares (verified empirically at 99%+ before tuning).

## Caveats

- The ITCH side is a download prefix (self-delimiting frames), so both
  sides were restricted to its time window; the NLS file itself was also
  a partial download (~502 MB decoded), covering well beyond the window.
- Comparison restricted to market center Q (Nasdaq execution system);
  TRF/L rows (5.4M in-window) are off-exchange reports outside an
  order-book diff's scope by construction.
