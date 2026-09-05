# AGENTS.md

Working agreement for any agent operating in this repo (mog).
The user's global rules in `~/AGENTS.md` always take precedence; the load-bearing subset is restated here so nothing depends on you having read it. Do not re-derive or soften these rules.

## Non-negotiables (restated from global)

1. Quality over cost. Never weigh development cost against maintainability, correctness, or scalability.
2. Never write em dashes or emojis unless explicitly asked.
3. One-line comments only. Docstrings are one line. No verbose comment blocks.
4. Run linter AND formatter before every commit. Never commit unformatted code.
5. Never add yourself as co-author in commits.
6. Prefer integration and end-to-end tests over basic unit tests.
7. Unrelated bugs you encounter get reported to the user immediately. Do not silently ignore them, do not silently expand scope; report, then ask.
8. If a library or tool is missing, prompt the user to install it. No workarounds, no cheaping out with substitutes.
9. Use git for everything. One logical unit per commit, terse messages.
10. When a pattern proves reusable, record it: skill if rare/conditional, else AGENTS.md notes.
11. When commiting, always make sure you update relevant md, docs, comments, docstrings.

## Repo identity and hard rules

- mog is a deterministic market simulator. Determinism IS the product (see DESIGN.md). Golden-digest anchors pin it; any change touching ordering, scheduling, or price/time priority must keep digests identical.
- Engine behavior changes ONLY on explicit mandate from the user. Default posture is zero engine-behavior change unless the user says otherwise.
- `results/` archives are append-only evidence. Reports there must be honest, caveats included; falsified hypotheses are recorded, never hidden.
- Real market data lives OUTSIDE the repo (typically `/tmp/opencode/day/`). Never commit captures or large artifacts.
- The user pushes between rounds and reports CI results back. Commit your unit, stop, wait. Do not start the next roadmap phase without an explicit go.

## Build and test gates

Both profiles must pass before claiming anything done:

    cmake -B build/portable -G Ninja -DMOG_PROFILE=portable -DCMAKE_C_COMPILER=g++-15 -DCMAKE_CXX_COMPILER=g++-15
    ctest --test-dir build/portable -j4

Same for `build/frontier` with `-DMOG_PROFILE=frontier`.

- Local machines may only have g++-15 even though CI covers gcc 13/14/15. Configure with g++-15 explicitly.
- The frontier profile enforces contracts (`MOG_PRE`) plus stricter warnings (`-Werror`, `-Wshadow`). Portable passing alone proves nothing: frontier has caught real bugs portable missed (band ceiling above INT32_MAX, shadowed test variable).
- After touching `tools/` or `tests/python/`: run `pytest tests/python`. The wheel-level suite under `python/tests/` needs an installed wheel in a fresh venv.
- `pre-commit run --all-files` must be green on every commit. Tool versions are pinned in lockstep across `.pre-commit-config.yaml` and `.github/workflows/lint.yml` (currently ruff 0.16.4, clang-format 21.1.8); bump all pins together.
- Perf claims need A/B against a pristine `HEAD` worktree with its own build
  dir (`cmake -S <worktree>`). Gate on Ir/op, latency percentiles, and
  digest equality; wall time alone proves nothing.

## Layout map

- `include/mog/`: headers carry most of the engine. `OrderBook.hpp` (price ladder; contract `hi_tick <= INT32_MAX`), `Replay.hpp` (book defaults: arena `1u<<22`, band `{0, INT32_MAX}`, pool `24576`; `Options::sized_for` scales arena and pool), `Simulate.hpp`, `Trades.hpp`, `SimRun.hpp`.
- `src/`: thin library + CLI entry (`main.cpp`).
- `python/`: pybind11 shim `_core`; API surface is deliberately small.
- `tools/`: CLI scripts (`sweep.py`, `calibrate.py`, `moglog.py`, `canonicalize-trades.py`, `unpriced_classify.cpp`, `nls_diff.py`, ...).
- `bench/race/`: participant harnesses for the four-way throughput race vs hftbacktest/nautilus.
- `tests/`: e2e binaries (one CTest each) + `tests/python/`.
- `results/`: evidence archives (race runs, ground-truth diffs), one dated directory per study.
- `docs/site/`: nine substantive pages; mkdocs wiring is pending on the roadmap.
- `DESIGN.md`: ranked doctrine, performance budgets, scope boundary, known external issues.

## Known traps

- `/tmp` is wiped externally and often: venvs, build dirs, and data vanish between sessions. Rebuild rather than assuming stale state is broken. The repo-local `tmp/` directory (gitignored) is the durable scratch space for fuzz corpora, downloads, and experiment artifacts.
- emi.nasdaq.com downloads stall (~1.6GB observed) and `curl --retry` RESTARTS truncated transfers. BinaryFILE frames are self-delimiting, so partial files remain valid; work with prefixes instead of re-downloading.
- NLS paired-sample methodology: NLS tapes lack ITCH match numbers (multiset diff on symbol/ts/price/shares), NLS sizes carry implied decimals (`--size-divisor`), Trade Report 'e' layout documented in `results/ground-truth/*/README.md`.
- nautilus_trader FLAT-side PositionOpened assert (`model/events/position.pyx:331`, reproducible under HEDGING and NETTING on 1.231.0) is THEIR bug (ledger item O3): document it, do not chase it as ours.
- Version is single-sourced: bump `project(VERSION)` in CMakeLists.txt only; pyproject reads it via the scikit-build-core regex provider.
- Line length is 100 everywhere: `.clang-format` and ruff must stay equal.

## Commit style

Terse, prefix form matching existing history: `feat(scope):`, `fix(scope):`, `docs:`, `chore:`, `ci:`. No co-author trailers, no verbose bodies beyond what a reviewer needs.
