#!/usr/bin/env bash
# Evaluation suite v0: correctness corpus + perf baselines over deterministic
# datasets, rendered against the PLAN.md performance budget table.
#
# Usage: suite/run_suite.sh [OUT_DIR]   (default results/)
# Everything is deterministic given the toolchain; absolute numbers are valid
# only per-machine - see docs/EVALUATION.md for the protocol.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
OUT="${1:-results}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
HOST_TAG="$(hostname -s 2>/dev/null || echo host)"
mkdir -p "$OUT"

echo "== environment =="
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')"
CORES="$(nproc)"
KERNEL="$(uname -r)"
CC_VER="$(${CXX:-g++} --version | head -1)"
echo "host=$HOST_TAG cpu=\"$CPU_MODEL\" cores=$CORES kernel=$KERNEL"
echo "compiler=$CC_VER"
{
  echo "host: $HOST_TAG"
  echo "cpu: $CPU_MODEL ($CORES cores)"
  echo "kernel: $KERNEL"
  echo "compiler: $CC_VER"
  echo "commit: $(git rev-parse HEAD)"
  echo "date: $STAMP"
} > "$OUT/env-$HOST_TAG.txt"

echo "== build =="
for prof in frontier portable; do
  cmake -S . -B "build/$prof" -G Ninja -DMOG_PROFILE=$prof > /dev/null
  cmake --build "build/$prof" -j "$(nproc)" > /dev/null
done

echo "== correctness: frontier =="
ctest --test-dir build/frontier --output-on-failure > "$OUT/ctest-frontier.log" 2>&1 \
  || { echo "frontier ctest FAILED"; tail -20 "$OUT/ctest-frontier.log"; exit 1; }
grep -E "tests passed" "$OUT/ctest-frontier.log" | tail -1

echo "== correctness: portable =="
ctest --test-dir build/portable --output-on-failure > "$OUT/ctest-portable.log" 2>&1 \
  || { echo "portable ctest FAILED"; tail -20 "$OUT/ctest-portable.log"; exit 1; }
grep -E "tests passed" "$OUT/ctest-portable.log" | tail -1

echo "== datasets =="
DATA_DIR="$(mktemp -d /tmp/mog-suite-data.XXXXXX)"
# Prices clustered in a ~400k-tick band: fits the book-e2e ladder page pool
# (uniform full-range prices would demand thousands of pages by design).
./build/frontier/tools/gen-sample/mog-gen-sample --seed 0xBEEFULL --count 1048576 \
  --price-center 4000000 --price-span 400000 \
  "$DATA_DIR/suite-1m.itch" > /dev/null
ls -l "$DATA_DIR/suite-1m.itch" | awk '{print "dataset:", $NF, $5, "bytes"}'

echo "== dataset-driven e2e: generated 1M capture =="
# The golden parser and book pipeline must agree with themselves and the
# reference book on the big deterministic dataset, not only on the small
# committed sample.
for t in mog-golden mog-book-e2e; do
  ./build/frontier/tests/$t "$DATA_DIR/suite-1m.itch" > /dev/null \
    || { echo "$t FAILED on 1M dataset"; exit 1; }
  echo "  $t ok"
done

echo "== perf baselines (pinned core, median of 3) =="
PIN=()
command -v taskset > /dev/null && PIN=(taskset -c "${MOG_SUITE_CORE:-2}")
for b in mog-bench mog-bench-book mog-bench-sched; do
  "${PIN[@]}" ./build/frontier/bench/$b \
    --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true \
    --benchmark_format=json > "$OUT/perf-$b-$HOST_TAG.json" 2>/dev/null
done

python3 - "$OUT" "$HOST_TAG" <<'PYEOF'
import json, sys
out, host = sys.argv[1], sys.argv[2]
rows = []
targets = [  # (benchmark prefix family, PLAN target description)
    ("BM_ParseStream", ">50M msg/s parser throughput"),
    ("BM_BookMixedFeed", ">20M events/s match/cancel"),
]
data = {}
for b in ("mog-bench", "mog-bench-book", "mog-bench-sched"):
    d = json.load(open(f"{out}/perf-{b}-{host}.json"))
    for x in d["benchmarks"]:
        if x.get("aggregate_name") == "median":
            nm = x["name"].split("_median", 1)[0]
            data[nm] = x
lines = ["| benchmark | cpu ns/iter | items/s | budget note |",
         "|---|---|---|---|"]
for nm in sorted(data):
    x = data[nm]
    ips = x.get("items_per_second", 0)
    note = ""
    if nm == "BM_ParseStream":
        note = f"target >50M msg/s -> {'MET' if ips > 50e6 else 'MISSED'}"
    elif nm == "BM_BookMixedFeed":
        note = f"target >20M events/s -> {'MET' if ips > 20e6 else 'MISSED'}"
    lines.append(f"| {nm} | {x['cpu_time']:.1f} | {ips:,.0f} | {note} |")
report = "\n".join(lines)
with open(f"{out}/perf-{host}.md", "w") as f:
    f.write(f"# Perf baseline {host}\n\n" + report + "\n")
print(report)
PYEOF

echo "== suite done: artifacts in $OUT =="
