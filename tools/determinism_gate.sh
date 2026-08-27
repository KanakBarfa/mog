#!/usr/bin/env bash
# Cross-compiler determinism gate: identical workloads through two builds
# (different compilers, same machine) must produce identical digests.
# Usage: determinism_gate.sh <buildDirA> <buildDirB>
set -euo pipefail

A=$(cd "$1" && pwd)
B=$(cd "$2" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail() {
    echo "DETERMINISM GATE FAILED: $*" >&2
    exit 1
}

[ -x "$A/mog" ] && [ -x "$B/mog" ] || fail "build dirs must contain the mog CLI"
"$A/mog" info | grep -q profile || fail "bad build A"
"$B/mog" info | grep -q profile || fail "bad build B"

gen="$A/tools/gen-sample/mog-gen-sample"

# Captures: three seeds, two price regimes (narrow band exercises page reuse,
# wide band spans ladder pages).
for seed in 0xBEEF 0x1234 0xDEADBEEF; do
    "$gen" --seed "$seed" --count 20000 --price-center 4000000 \
        --price-span 400000 "$work/narrow_$seed.itch" >/dev/null
    "$gen" --seed "$seed" --count 20000 --price-center 2000000000 \
        --price-span 2000000000 "$work/wide_$seed.itch" >/dev/null
done

digest_of() { # bin file -> digest field from --json replay summary
    "$1" replay "$2" --json | sed -n 's/.*"digest" *: *"\([^"]*\)".*/\1/p'
}

status=0
for seed in 0xBEEF 0x1234 0xDEADBEEF; do
    for regime in narrow wide; do
        f="$work/${regime}_$seed.itch"
        da=$(digest_of "$A/mog" "$f")
        db=$(digest_of "$B/mog" "$f")
        if [ "$da" != "$db" ]; then
            echo "MISMATCH replay $regime/$seed: $da vs $db" >&2
            status=1
        fi
    done
done

# Simulator battery: scripted flow with latencies, jitter, fees; the trace
# digest covers fills and decisions.
cat > "$work/script_basic.csv" <<'EOF'
kind,ts_ns,side,price_ticks,qty,ref
ext_add,100,B,998,100,1
ext_add,100,S,1010,150,2
strat_limit,200,B,999,10,5001
strat_ioc,300,S,998,25,5002
trade,400,S,998,60,9001
ext_add,500,B,990,80,3
strat_cancel,600,B,998,1,5001
strat_limit,700,S,1004,15,5003
trade,800,B,1004,15,9002
EOF
cat > "$work/script_jitter.csv" <<'EOF'
kind,ts_ns,side,price_ticks,qty,ref
ext_add,100,B,1998,400,11
ext_add,100,S,2002,380,12
strat_limit,150,B,1997,50,7001
strat_limit,150,S,2003,50,7002
trade,250,S,1998,120,9101
trade,350,B,2002,110,9102
strat_cancel,450,B,1997,1,7001
strat_limit,460,B,1996,50,7003
trade,550,S,1997,30,9103
strat_ioc,650,B,2010,40,7004
EOF

simrun_case() { # label script extra-args...
    local label=$1 script=$2
    shift 2
    local da db
    da=$("$A/mog" simrun "$script" --json "$@" | sed -n 's/.*"digest_high" *: *"\([^"]*\)".*/\1/p')
    db=$("$B/mog" simrun "$script" --json "$@" | sed -n 's/.*"digest_high" *: *"\([^"]*\)".*/\1/p')
    if [ -z "$da" ] || [ -z "$db" ]; then
        echo "MISSING DIGEST $label (a='$da' b='$db')" >&2
        status=1
        return
    fi
    if [ "$da" != "$db" ]; then
        echo "MISMATCH simrun/$label: $da vs $db" >&2
        status=1
    fi
}

simrun_case basic "$work/script_basic.csv" --seed 7
simrun_case jitter "$work/script_jitter.csv" --seed 42 --latency-ns 200 \
    --jitter uniform:100 --maker-fee-bps -2 --taker-fee-bps 5
simrun_case depletion "$work/script_jitter.csv" --seed 9 --depletion 0.5,0.3 \
    --latency-ns 150

[ "$status" -eq 0 ] && echo "determinism gate OK: $(basename "$A") == $(basename "$B")"
exit "$status"
