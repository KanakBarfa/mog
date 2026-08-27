# Evaluation suite

Versioned, rerunnable evidence. Every fidelity or performance claim in the
docs cites something this directory can reproduce.

## Arms

- **Cross-engine race** (`run-race.sh`): identical workload bytes through mog,
  hftbacktest (plain + njit) and nautilus adapters from `bench/race/`, plus
  per-order fill agreement (`agree.py`) against exact-FIFO ground truth.
  Promoted into the standing suite; results are committed, not regenerated away.
- **Ground-truth trade validation** (`ground-truth.sh`): reconstructs prints
  from E/C messages over a public ITCH capture and diffs them against a
  converted official Daily Trade file via `mog trades --diff`. Divergence
  count ≈ 0 is the end-to-end fidelity claim.

## Convention

Each arm writes into `results/<arm>/<date>-<gitsha>/`. A claim without a
results directory behind it is marketing.
