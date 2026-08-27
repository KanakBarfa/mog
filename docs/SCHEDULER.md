# Scheduler design note

Discrete-event ordering for replay: events carry an arbitrary nanosecond
timestamp and pop in total order regardless of push order.

## Ordering semantics

Total order is lexicographic on `(ts, seq)` where `seq` is a monotonically
increasing insertion counter assigned by `push()`. Because `seq` values are
unique, equal timestamps pop strictly in push order and no comparison is ever
ambiguous; the pop stream is a pure function of the op sequence.

- `push(ts, value)` schedules at any timestamp; monotonic pushes are not required.
- `pop_min()` returns the payload by value; slots are recycled raw.
- `peek_min()` / `peek_min_ts()` inspect without mutating.
- `cancel(handle)` removes a pending event; it returns `false` for stale
  handles (already popped or cancelled) and bad indices, mirroring the book's
  recoverable-error style.
- Handles are generational (`GenHandle`): release bumps the slot generation,
  so captured handles can never address recycled events.
- `clear()` empties the schedule, bumps generations of live slots, and resets
  the sequence counter.

## Layout

```
Scheduler<T>
  Slot[capacity]      raw array, placement-new'd once in the ctor
    { T value; u64 seq; u32 generation; u32 heap_pos; u32 free_next; bool live }
  heap_               vector<HeapEntry{u64 ts; u64 seq; u32 slot}>, reserved to capacity
  free_head_          intrusive free-list head over slots (LIFO reuse)
  next_seq_           tie-break counter
```

Heap entries are self-contained keys: sift comparisons never chase slot
memory. `heap_pos` backlinks make cancellation O(log n) via remove-at-position
(swap-with-last, then sift up or down depending on the parent comparison).

## Complexity

| Op | Cost |
|---|---|
| push | O(log n) sift-up |
| pop_min | O(log n) sift-down |
| cancel | O(log n) |
| peek_min | O(1) |

Steady state performs zero allocations: storage is sized at construction and
`push` is precondition-guarded against overflow (`MOG_PRE(size < capacity)`).

## Contracts

- `push`: capacity not exceeded; free slot available.
- `pop_min`/`peek_*`: heap non-empty.
- `pop_min` postcondition: every remaining entry sorts no earlier than the
  popped one.
- `audit()`: full structural check (heap ordering, position backlinks, seq
  agreement, free-list length); O(n), used by tests every 997 ops.

The contracts caught two real defects during development: uninitialized
free-slot metadata (raw `operator new` memory read through `audit`; fixed by
placement-new'ing every slot in the constructor - the same latent issue was
fixed in `Arena`), and an unspecified-argument-evaluation-order bug in the
test harness drain loop that let `pop_min` run before `peek_min_ts`.

## Determinism protocol

`scheduler_diff` phase 3 replays a seeded 300k-op corpus five times in fresh
schedulers, hashing every popped `(ts, payload)` into a SHA-256 trace
(`Sha256.hpp`, NIST known-answer vectors checked first). All digests must be
identical:

```
sha256 trace: 1a7f5163439364e2c0bb35fb54491d227185000ede3a8cc617b8c17ff0845ae3
```

## Measured performance (i5-7500T, 4-core part)

Exit-criterion runner (`mog-bench-sched <events>`, `MOG_SCHED_LIVE` sets the
live window):

| Live window | Events/s | Regime |
|---|---|---|
| 65,536 | 4.49M | heap fits L2/L3; ~223 ns/event |
| 4,194,304 | 1.00M | 96 MB heap; pop-path sift-down walks random lines |

gbenchmark steady-state push+pop at a 64k window agrees (~2.7M events/s).

## Timing wheel: implemented, measured, adopted for large windows

`TimingWheel.hpp` is a full hierarchical timing wheel: 11 levels of 64 buckets
over the base-64 digits of the nanosecond timestamp, O(1) routing by highest
differing digit, O(1) cancellation through intrusive bucket chains, and
watermark demotion cascades bounded by one level-step per event per advance.
Its structural invariant makes ordering exact: when the watermark reaches an
event's timestamp the event has been demoted into level 0's current bucket,
which therefore holds precisely the events due at that nanosecond; equal-tick
stability comes from seq-ordered emission with an out-of-order fallback scan.

The wheel adds one contract the heap does not have: pushes must satisfy
`ts >= watermark` (no scheduling into the past). Replay feeds are naturally
monotonic, so this costs nothing in practice and the engines share the
`EventHandle` type and pop-stream semantics.

Measured on i5-7500T (`mog-bench-sched <events>`, non-decreasing jitter feed,
4M live window):

| Engine | Events/s | Notes |
|---|---|---|
| B-heap, adversarial shuffle | 0.98M | sift-down random walks dominate |
| B-heap, near-sorted | 4.01M | near-sorted feeds flatter the heap too |
| Wheel, near-sorted | **8.56M** | 2.1x over heap; identical pop stream |

At cache-resident windows (<=262k live) the two tie within noise on this part:
the heap's dense 24-byte entries beat the wheel's 48-byte slots plus chain
chasing there. Both runs of the exit runner also produced byte-identical sink
hashes, independently confirming pop-stream equality at production scale.

Verification: `scheduler_diff` runs unit probes, differential fuzz, and the
SHA-256 harness against both engines; the cross-engine mirror phase requires
identical pop streams under identical op streams, and phase 3 requires both
engines to agree on the trace digest:

```
sha256 trace: b42d7b00c969f45faac681d52a0617f6b5be8c8f926c543f2ce207898641a6e2
```

Decision: keep both engines. The B-heap remains the general-purpose
reference (no push-order constraint); the wheel serves monotonic feeds and
wins wherever live windows outgrow L3.

## M3 exit status

- 100M events streamed allocation-free: yes (windowed runner).
- Determinism green across repeated runs: yes (5x identical SHA-256 per
  engine, engines mutually consistent).
