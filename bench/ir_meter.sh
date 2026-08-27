#!/bin/sh
# Reports instructions retired per successful book op via callgrind.
# Usage: bench/ir_meter.sh [binary] [samples]
# Machine independent: same binary and input stream give identical counts.
set -e
BIN=${1:-build/frontier/bench/mog-bench-book-ir}
N=${2:-100000}
OUT=$(mktemp)
RUNLOG=$(mktemp)
trap 'rm -f "$OUT" "$RUNLOG"' EXIT

valgrind --tool=callgrind --callgrind-out-file="$OUT" --collect-atstart=no \
    "$BIN" "$N" >"$RUNLOG" 2>/dev/null

IR=$(callgrind_annotate "$OUT" 2>/dev/null | awk '/PROGRAM TOTALS/ {gsub(",", "", $1); print $1; exit}')
OK=$(grep -o "of [0-9]* attempted" "$RUNLOG" | grep -o "[0-9]*")

if [ -z "$IR" ] || [ -z "$OK" ]; then
    echo "ir_meter FAIL: could not parse totals" >&2
    exit 1
fi
awk -v ir="$IR" -v ok="$OK" 'BEGIN { printf "ir_per_op=%d (%d total / %d successful ops)\n", ir / ok, ir, ok }'
