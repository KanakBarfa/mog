#!/usr/bin/env bash
# Asm-parity snapshot: extract the demangled disassembly of designated hot
# kernels from built binaries and emit an ordered text snapshot. CI compares
# against the committed golden; a diff means codegen moved and either a
# performance review or a deliberate golden refresh (--refresh) is due.
#
# The build must use a fixed ISA target (-DMOG_ARCH=x86-64-v3), never
# -march=native: native would make snapshots host-CPU dependent.
#
# Usage: tools/asm_snapshot.sh <build-dir> <snapshot-file> [--refresh]
set -euo pipefail

BUILD="$1"
OUT="$2"
cd "$(git rev-parse --show-toplevel)"

BIN="$BUILD/bench/mog-bench"
[[ -x "$BIN" ]] || { echo "missing $BIN" >&2; exit 2; }

# Demangled symbol substrings that define the tracked kernel set.
PATTERNS=(
  "mog::parse_itch"
)

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

# nm gives mangled names; objdump --disassemble= takes exactly those.
nm --defined-only "$BIN" | awk '$2 == "T" || $2 == "t" { print $3 }' > "$TMP.syms" 2>/dev/null ||
  nm -g --defined-only "$BIN" | awk '$2 == "T" { print $3 }' > "$TMP.syms"

: > "$TMP.out"
while IFS= read -r mangled; do
  demangled="$(echo "$mangled" | c++filt)"
  keep=0
  for p in "${PATTERNS[@]}"; do
    [[ "$demangled" == *"$p"* ]] && keep=1 && break
  done
  if [[ "$keep" -eq 1 ]]; then
    echo "=== $demangled" >> "$TMP.out"
    objdump -d --no-show-raw-insn --disassemble="$mangled" "$BIN" |
      sed 's/^ *//' | grep -vE '^(Disassembly|file format|[0-9a-f]+ <)' >> "$TMP.out" || true
  fi
done < "$TMP.syms"

if [[ ! -s "$TMP.out" ]]; then
  echo "no tracked symbols found in $BIN; pattern list needs updating" >&2
  exit 2
fi

if [[ "${3:-}" == "--refresh" ]]; then
  mkdir -p "$(dirname "$OUT")"
  cp "$TMP.out" "$OUT"
  echo "refreshed $OUT ($(wc -l < "$OUT") lines, sha256 $(sha256sum "$OUT" | cut -c1-16))"
else
  if [[ ! -f "$OUT" ]]; then
    echo "golden $OUT missing; generate with: tools/asm_snapshot.sh <build> $OUT --refresh" >&2
    exit 1
  fi
  if diff -u "$OUT" "$TMP.out" > "$TMP.diff"; then
    echo "asm parity ok: $(sha256sum "$TMP.out" | cut -c1-16)"
  else
    echo "ASM PARITY FAILURE - codegen changed for tracked kernels:" >&2
    head -40 "$TMP.diff" >&2
    echo "If intentional (compiler bump / source change), refresh via:" >&2
    echo "  tools/asm_snapshot.sh <build-dir> $OUT --refresh" >&2
    exit 1
  fi
fi
