#!/usr/bin/env bash
# Cross-engine differential arm of the evaluation suite (G12): reruns the
# race harness over the canonical feed. Results land versioned under
# results/race/<date>-<gitsha>/ so agreement claims stay reproducible.
set -euo pipefail
cd "$(dirname "$0")/.."
SHA=$(git rev-parse --short HEAD)
OUT="results/race/$(date +%Y%m%d)-${SHA}"
mkdir -p "${OUT}"
PY="${MOG_PY:-python3}"
if [ ! -x build/portable/mog ]; then
    echo "build first: cmake -B build/portable && cmake --build build/portable" >&2
    exit 1
fi
"${PY}" -m bench.race.feedgen --out bench/race/out \
    --quotes --npy
g++-15 -std=c++23 -O2 -march=native -DMOG_PROFILE_FRONTIER=1 \
    -I include -I build/p23/generated \
    bench/race/participants/mog_native.cpp -o bench/race/out/mog_native
MOG_PYLIB=build/py-portable/pylib "${PY}" -m bench.race.run_race \
    --runs 7 --cooldown 20
cp bench/race/out/race_results.json "${OUT}/"
cp bench/race/out/*.json "${OUT}/" 2>/dev/null || true
echo "results -> ${OUT}"
