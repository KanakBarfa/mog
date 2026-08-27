#!/usr/bin/env bash
# Perf regression gate: build BASE and HEAD bench binaries, run tracked
# kernels in interleaved rounds, aggregate median-of-round-medians, and fail
# on >threshold slowdown. Noise on shared runners shifts single rounds; a real
# regression moves every round.
#
# Usage: tools/perfgate.sh [BASE_REF] [--threshold T] [--rounds R]
#   BASE_REF defaults to the merge-base with origin/main.
#   MOG_GATE_DIR overrides the scratch dir (default .perf-gate/ in-workspace:
#   hosted-runner /tmp may be mounted noexec).
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
BASE_REF="${1:-}"
THRESHOLD=1.10
ROUNDS=3
PIN_CORE="${MOG_GATE_CORE:-2}"
if [[ -n "$BASE_REF" ]]; then shift; fi
while [[ $# -gt 0 ]]; do
  case "$1" in
    --threshold) THRESHOLD="$2"; shift 2 ;;
    --rounds) ROUNDS="$2"; shift 2 ;;
    *) echo "unknown arg $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$BASE_REF" ]]; then
  BASE_REF="$(git merge-base HEAD origin/main 2>/dev/null || echo HEAD)"
fi
HEAD_SHA="$(git rev-parse HEAD)"
BASE_SHA="$(git rev-parse "$BASE_REF")"
echo "perf gate: base=$BASE_SHA head=$HEAD_SHA rounds=$ROUNDS threshold=$THRESHOLD core=$PIN_CORE"

# Scratch lives inside the workspace by default: hosted-runner /tmp may be
# mounted noexec, which would kill every freshly built binary with a bare
# exit 126. Override with MOG_GATE_DIR.
GATE_DIR="${MOG_GATE_DIR:-$PWD/.perf-gate}"
rm -rf "$GATE_DIR"
mkdir -p "$GATE_DIR"
echo "workdir: $GATE_DIR"

BENCHES=(mog-bench mog-bench-book mog-bench-sched)

# Tracked kernel subsets: representative throughput shapes only. Full suites
# remain available locally via the plain bench binaries.
FILTERS=(
  '^BM_ParseStream$|^BM_DecodeAddOrder(Cpp|Asm)$'
  '^BM_BookMixedFeed$|^BM_BookAddRemoveCycle$'
  '^BM_CancelHeavy|^BM_DrainSorted/262144$|^BM_NearSorted_Wheel'
)

build_side() { # $1 = sha, $2 = dir
  local worktree="$GATE_DIR/wt-$1"
  if [[ ! -d "$worktree" ]]; then
    git worktree add --detach "$worktree" "$1" > /dev/null
  fi
  cmake -S "$worktree" -B "$2" -G Ninja -DMOG_PROFILE=frontier > /dev/null 2>&1
  cmake --build "$2" --target "${BENCHES[@]}" -j "$(nproc)" > /dev/null 2>&1
}

run_one() { # $1 = build dir, $2 = out prefix, $3 = index into BENCHES/FILTERS
  local pin=()
  if command -v taskset > /dev/null && taskset -c "$PIN_CORE" true > /dev/null 2>&1; then
    pin=(taskset -c "$PIN_CORE")
  else
    echo "NOTE: core pinning unavailable; running unpinned" >&2
  fi
  local b="${BENCHES[$3]}"
  local attempt rc=0
  for attempt in 1 2; do
    # One retry absorbs transient runner flakes; a deterministic crash fails
    # both attempts and is reported with its stderr tail below.
    if "${pin[@]}" "$1/bench/$b" \
        --benchmark_filter="${FILTERS[$3]}" \
        --benchmark_repetitions=3 \
        --benchmark_min_time=0.25s \
        --benchmark_report_aggregates_only=true \
        --benchmark_format=json > "$2-$b.json" 2> "$2-$b.err"; then
      return 0
    fi
    rc=$?
    sleep 2
  done
  echo "WARNING: $b exited code $rc ($2); stderr tail:" >&2
  tail -5 "$2-$b.err" 2>/dev/null | sed 's/^/  | /' >&2 || true
  return 0 # comparator reports the missing data as a failure
}

echo "building base..."
build_side "$BASE_SHA" "$GATE_DIR/build-base"
echo "building head..."
build_side "$HEAD_SHA" "$GATE_DIR/build-head"

for r in $(seq 1 "$ROUNDS"); do
  echo "round $r/$ROUNDS"
  for i in "${!BENCHES[@]}"; do
    run_one "$GATE_DIR/build-base" "$GATE_DIR/r${r}-base" "$i"
  done
  for i in "${!BENCHES[@]}"; do
    run_one "$GATE_DIR/build-head" "$GATE_DIR/r${r}-head" "$i"
  done
done

ARGS=()
for r in $(seq 1 "$ROUNDS"); do
  for i in "${!BENCHES[@]}"; do
    ARGS+=("$GATE_DIR/r${r}-base-${BENCHES[$i]}.json" \
           "$GATE_DIR/r${r}-head-${BENCHES[$i]}.json")
  done
done

STATUS=0
python3 tools/aggregate_perf.py --threshold "$THRESHOLD" "${ARGS[@]}" || STATUS=1

git worktree remove --force "$GATE_DIR/wt-$BASE_SHA" > /dev/null 2>&1 || true
git worktree remove --force "$GATE_DIR/wt-$HEAD_SHA" > /dev/null 2>&1 || true

if [[ "$STATUS" -ne 0 ]]; then
  echo "PERF GATE: FAILED"
  exit 1
fi
echo "PERF GATE: PASSED"
