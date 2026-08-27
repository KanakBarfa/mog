# Real-capture validation - NASDAQ TotalView-ITCH 5.0 sample

Source: emi.nasdaq.com public sample `12302019.NASDAQ_ITCH50.gz`
(first ~210MB compressed => first 7m47s of the 2019-12-30 session,
528,457,355 payload bytes, 16.1M frames). Data is license-restricted;
only these derived summaries are committed.

## Replay (session model + L3 book)

decoded 14,762,222 | skipped(session frames) 1,378,952
halt windows 11 | NOII snapshots 1,073,085 | close crosses n/a (prefix ends 09:37)
wall ~4s (~130 MB/s) | maxRSS ~2.1 GB | live orders at end 1,693,707

## Trade reconstruction (E/C -> prints)

prints 222,968 | unpriced 0 (0.00%) | halted-window prints 0

## How the "unpriced executions" were eliminated (was 5,180 / 2.3%)

Every mystery miss turned out to be silent capacity starvation in book
defaults - two independent caps, each starving books whose later executions
then surfaced as unknown-order prints. Root-caused by counting errors on
BOTH sides of the stream (`tools/unpriced_classify.cpp`): executions tell
only half the story; the rejected adds never appear there at all.

1. **$1,200 default band cap (~85% of the gap).** AMZN traded at $1,872:
   every add bounced off `out_of_band`, the symbol reconstructed ZERO
   prints, and its executions became mystery misses. Defaults now span the
   full ITCH wire price domain; regression pins $2000 reconstruction.
2. **Page-pool starvation (the entire remainder).** With the band widened,
   21,090 adds still failed `page_exhausted` under an 8192-page pool:
   a full-symbol-universe prefix spans more distinct 1024-tick price
   neighborhoods than that. Pool raised to 24,576 pages and made
   input-adaptive via `Options::sized_for`, so fixtures and short captures
   stay light while day-scale inputs get what they demand.

Earlier hypotheses (cancel-vs-execute races, non-displayed liquidity) were
FALSIFIED on this capture: once both caps were fixed they vanished entirely.
What remains is a fully-priced reconstruction - 0 divergences from the
displayed book over 222,968 prints.

Reproduction: `mog trades <capture> --lenprefix --json`; the classifier
emits per-locate stats for external cross-checks.

## Known follow-up

O1's exact official-file diff awaits a license-clean NASDAQ Daily Trades
file; free consolidated daily bars bound-check reconstruction via
`tools/bounds_check.py` (independent venue, one-sided tolerance).
