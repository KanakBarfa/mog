# Contributing to MOG

Short version: determinism is the product. If your change risks it, you will
be asked to prove it doesn't. Everything else is negotiable.

## Ground rules

1. **Determinism anchors are load-bearing.** Parser golden hash
   (`0x308e35b105cfe9d0`), scenario digests (`498cda3c...`), fuzz traces - if
   one moves, your change altered observable behavior. That is sometimes the
   point; then the anchor update goes in the same commit *with justification*.
2. **One commit per logical unit**, terse message, rationale in the body not
   the title.
3. **Tests ride with the change** that needs them - regression tests land in
   the same commit as the fix.
4. **No new dependencies** without an RFC. Vendored polyfills under
   `include/mog/polyfill/` are how standard-library gaps get filled (see
   [the C++26 ledger](docs/site/cpp26-ledger-explainer.md)).

## Development workflow

```sh
cmake -B build/frontier -G Ninja -DMOG_PROFILE=frontier
cmake --build build/frontier -j
ctest --test-dir build/frontier

# The consumer path your change must also survive (wheels build this):
cmake -B build/p23 -G Ninja -DMOG_PROFILE=portable -DMOG_CXX_STANDARD=23
cmake --build build/p23 -j && ctest --test-dir build/p23
```

Sanitizers before submitting anything touching memory ownership or arenas:

```sh
cmake -B build/san -DMOG_PROFILE=frontier -DMOG_SANITIZE="address;undefined"
```

Style: match the surrounding code. Warnings-as-errors on frontier; no
exceptions/RTTI except the Python binding TU.

## When you need an RFC

Issues carry discussion; RFCs carry **decisions**. Write one when changing:

- parser wire semantics or framing,
- book matching/invariant behavior,
- fill-model assumptions or guarantees (anything in FILLMODEL.md),
- public API surface (C++ headers or the Python shim),
- the compatibility floor (new toolchain requirement, new polyfill policy),
- performance-gate protocol or thresholds.

Process:

1. Open an issue titled `rfc: <topic>` describing the problem first, the
   solution second.
2. If consensus forms, copy `docs/rfcs/000-template.md` to
   `docs/rfcs/NNNN-<slug>.md` (next number) and open a PR.
3. The PR is the review vehicle. Merge = accepted; closing unmerged =
   declined (with reasons recorded in the issue).
4. Accepted RFCs get implemented in follow-up commits referencing `RFC-NNNN`.

## Issue triage conventions

Triage happens in the open; maintainers apply labels within a few days.

| Label | Meaning |
|---|---|
| `determinism` | any nondeterminism report - treated as P0; a repro beats a reproach |
| `perf-regression` | slowdown claims must attach gate output or a rerun showing consistency (single reds on shared runners happen) |
| `correctness` | wrong fills/state vs documented semantics; needs minimal script, ideally digest-reproducible |
| `compat` | toolchain/platform matrix issues; verify against `mog-probe` output |
| `docs` | includes "this bias/limitation was not on the reference page" |
| `rfc` | proposals awaiting the process above |

Severity defaults: `determinism` > `correctness` > `perf-regression` >
`compat` > `docs`. A `perf-regression` without numbers and a `correctness`
without a repro are questions, not bugs - they get converted, not closed.

Performance reports: state hardware (`results/env-*.txt` format), corpus,
and command. Numbers without machine context are unreadable by policy.
