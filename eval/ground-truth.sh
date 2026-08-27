#!/usr/bin/env bash
# Ground-truth validation workflow (G4). Requires two inputs the user must
# supply (license-clean public files, not redistributed here):
#   $ITCH_CAPTURE  raw ITCH 5.0 day capture (e.g. NASDAQ FTP sample)
#   $TRADE_FILE    official daily trade file converted to canonical CSV:
#                  match_number,ts_ns,price_ticks,shares  (sorted by match)
set -euo pipefail
cd "$(dirname "$0")/.."
: "${ITCH_CAPTURE:?set ITCH_CAPTURE}"
: "${TRADE_FILE:?set TRADE_FILE (canonical CSV)}"
SHA=$(git rev-parse --short HEAD)
OUT="results/ground-truth/$(date +%Y%m%d)-${SHA}"
mkdir -p "${OUT}"
./build/portable/mog trades "${ITCH_CAPTURE}" --csv "${OUT}/ours.csv"
python3 tools/canonicalize-trades.py "${TRADE_FILE}" "${OUT}/ref.csv"
./build/portable/mog trades "${ITCH_CAPTURE}" --csv /dev/null \
    --diff "${OUT}/ref.csv" | tee "${OUT}/report.txt"
cp "${OUT}/ours.csv" "${OUT}/ref.csv" . 2>/dev/null || true
echo "results -> ${OUT}"
