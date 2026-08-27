#!/usr/bin/env bash
# Differential asm-parity check: build the parser bench at the merge-base and
# at HEAD with a fixed ISA target, snapshot the hot kernels' disassembly from
# both, and require equality. Same job, same compiler, same flags: any diff is
# attributable to the source change under review.
#
# Set ASM_PARITY_ALLOW=1 to acknowledge an intentional codegen change and pass.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
BASE_SHA="$(git merge-base HEAD origin/main 2>/dev/null || git rev-parse HEAD~1)"
HEAD_SHA="$(git rev-parse HEAD)"

if [[ "$BASE_SHA" == "$HEAD_SHA" ]]; then
  echo "asm parity: no base to compare (direct push); skipping"
  exit 0
fi

ARCH="${ASM_PARITY_ARCH:-x86-64-v3}"
DIR="$(mktemp -d /tmp/mog-asm.XXXXXX)"
trap 'rm -rf "$DIR"' EXIT

snapshot() { # $1 = sha, $2 = name
  local wt="$DIR/wt-$2"
  git worktree add --detach "$wt" "$1" > /dev/null 2>&1
  cmake -S "$wt" -B "$DIR/build-$2" -G Ninja \
    -DMOG_PROFILE=frontier -DMOG_ARCH="$ARCH" > /dev/null 2>&1
  cmake --build "$DIR/build-$2" --target mog-bench -j "$(nproc)" > /dev/null 2>&1
  tools/asm_snapshot.sh "$DIR/build-$2" "$DIR/snap-$2.txt" --refresh > /dev/null
  git worktree remove --force "$wt" > /dev/null 2>&1 || true
}

echo "asm parity: base=$BASE_SHA head=$HEAD_SHA arch=$ARCH"
snapshot "$BASE_SHA" base
snapshot "$HEAD_SHA" head

if diff -u "$DIR/snap-base.txt" "$DIR/snap-head.txt" > "$DIR/parity.diff"; then
  echo "asm parity ok ($(wc -l < "$DIR/snap-head.txt") lines identical)"
else
  echo "ASM PARITY: tracked kernel codegen changed:" >&2
  head -60 "$DIR/parity.diff" >&2
  if [[ "${ASM_PARITY_ALLOW:-0}" == "1" ]]; then
    echo "ASM_PARITY_ALLOW=1 set; acknowledging intentional change"
    exit 0
  fi
  echo "Review the diff. If the codegen change is intended, re-run this job" >&2
  echo "with ASM_PARITY_ALLOW=1 (document it in the PR description)." >&2
  exit 1
fi
