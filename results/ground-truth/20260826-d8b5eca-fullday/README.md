# O1 at full scale: complete-session reconstruction vs official Nasdaq Last Sale 4.0

**The full trading day, not a prefix.** Source: the same designed pair as
the prefix study - `itch50_05_15.gz` (13.05 GB, 30.69 GB raw) against
`nls40_05_15.gz` (1.64 GB, 5.09 GB raw), both fetched whole from
emi.nasdaq.com and size-verified. Both sides span 04:00 through market close.

## Headline numbers

| Metric | Value |
|---|---|
| Our prints (E/C book executions), full session | **18,021,953** |
| Internally unpriced E/C | **909 (0.0050%)** |
| Q-center tape rows in window | 26,311,401 |
| Exact record containment (ours vs tape) | **99.957%** (7,776 misses) |
| Matched volume share of tape volume | 45.32% |
| Our-side volume deficit | **63,472 shares** (~0.0017% of tape volume) |
| Tape-surplus volume (no book counterpart) | 2.004B shares (54.68%), 898,670 buckets |

## What the full day exposed that the prefix hid

1. **Arena exhaustion was real and is fixed.** The prefix (04:00-09:36)
   peaked under the default 4M live-order arena; the full day blew through
   it: 13,380,869 adds rejected `arena_full`, producing 1.68M unpriced
   executions (10.3%). With `sized_for` scaled to 16.7M slots: 0 arena
   failures, unpriced drops to 909. Committed separately (`d8b5eca`).
2. **Record containment improved with scale**: 99.94% (prefix) ->
   99.957% (full day).
3. **The per-bucket volume metric collapsed by construction.** Prefix:
   94.54% of (symbol,price) buckets reconciled exactly. Full day: 16.29%
   exact, 18.18% within 2%. This is NOT new infidelity - our-side deficit
   is 63k shares across 3.67B; it means most full-day buckets mix matched
   volume with tape-only surplus, so a 2% window rarely closes. The honest
   full-day lens is record containment plus the volume split above.

## Residual decomposition (consistent with the prefix study)

- **Tape-only liquidity dominates the surplus**: 8.30M Q-center records
  (2.00B shares) have no ITCH order-lifecycle counterpart. The prefix
  study verified this class directly (98.3% round-lot+ volume); the full
  day multiplies it, consistent with non-displayed liquidity reporting.
- **Report-time aggregation**: verified mechanism from the prefix run
  (RKLZ: 32 fills beside one 76,086-share report) applies to the 7,776
  ours-without-ref rows.
- **Timestamp drift**: sub-percent of misses on the prefix; unchanged.

## Reproduction

```
# fetch + decompress (see tmp/fullday/fetch.sh in the session log)
g++-15 -std=c++26 -O2 -I include -I build/portable/generated \
    -DMOG_PROFILE_PORTABLE=1 -DMOG_CXX_STANDARD=26 tools/unpriced_classify.cpp
./a.out itch50_05_15.itch stats.csv prints.csv light     # ours side
python3 tools/nls_diff.py nls40_05_15.raw prints.csv \
    --size-divisor 1000000 --buckets-prefix buckets      # official side
```

`light` mode drops ref-fate maps (hundreds of millions of refs make them
cost tens of GB for diagnostics the headline numbers never read); error
histograms bucket as unseen. The classifier maps lazily - `MAP_POPULATE`
prefaults files larger than RAM.

## Caveats

- md5 reference file was unavailable server-side for this artifact;
  verification is exact byte-size only.
- Comparison restricted to market center Q; L/TRF rows (59.45M in window)
  are off-exchange reports outside an order-book diff's scope.
- The classifier ran in light mode; the 909 unpriced carry no stale-vs-
  unknown subdivision (prefix forensics covered that taxonomy).
