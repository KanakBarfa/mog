# Chapter 09: Scheduling, Concurrency & Tooling

**What you will learn:** how millions of future events stay perfectly
ordered (timing wheel), how threads hand off data without locks (SPSC
ring), how the debugger rewinds time, why file reading is free-looking,
and how Python talks to C++.

---

## 1. The big picture: a simulation is a to-do list

A replay is fundamentally: pop the next-dated event from a giant priority
queue, process it, maybe schedule more events. With hundreds of millions
of entries, the queue itself must be near-free. This chapter is the
supporting cast that makes that possible.

## 2. The hierarchical timing wheel

Binary heaps insert in O(log N) with pointer-chasing at every level.
mog's scheduler ([TimingWheel.hpp](../include/mog/TimingWheel.hpp))
instead exploits a clock metaphor:

Picture 11 nested clock faces. Each face has **64 slots**; one step on an
outer ring advances the whole inner ring by one slot - exactly like an
odometer where each digit is base-64 rather than base-10. The rings
together span $64^{11}$ nanoseconds - over 500 years of future.

Scheduling is pure arithmetic: write the event's nanosecond timestamp in
base-64 and look at where it first differs from "now" - that digit names
the ring, the remaining digits name the slot. O(1), no comparisons, no
allocation (chapter 04's rules apply here too).

As simulated time advances, events whose ring comes due demote inward,
one ring per revolution, until they land on the innermost ring - which
holds only events within the current 64-nanosecond window, executed in
exact timestamp order. Ties break deterministically by sequence number.

## 3. The SPSC ring: two workers, one conveyor belt

Logging telemetry must not slow the engine, so a background thread does
the writing while the engine thread produces. Two threads sharing memory
normally need locks - which are slow and, worse, nondeterministic in
timing.

The lock-free answer ([SpscRing.hpp](../include/mog/SpscRing.hpp)) works
when exactly one producer pushes and one consumer pops (**SPSC** -
Single-Producer, Single-Consumer): a fixed circular buffer where the
producer owns advancing the *head* index, the consumer owns the *tail*,
and neither ever writes the other's index. Each publishes its update with
C++ atomic `release` semantics ("everything I wrote is now visible") and
reads the other's with `acquire` ("I see everything they published").
Those two orderings are the entire protocol: no locks, no waiting, and
the engine never blocks on the logger.

## 4. Time-travel debugging

When a fill looks wrong, you want to ask "what was the book at that exact
moment - and what if I had done something else?"
([TimeTravel.hpp](../include/mog/TimeTravel.hpp)):

- The session journals every causal operation (`SimOp`) - adds, cancels,
  strategy decisions.
- Snapshots allow stepping the replay **backward**, inspecting any
  order's queue state at any past tick.
- `fork_at(k)` branches reality at operation k: rerun the suffix with a
  different decision (placed 100 microseconds later, different size) and
  diff which fills changed and why.

Counterfactual replay doubles as a regression weapon: engine changes are
rerun against a corpus of forks, so any behavioral drift surfaces as a
diverged fork rather than a silent number change.

## 5. Zero-copy file reading: mmap

Reading a 30 GB exchange recording with ordinary file calls means the
kernel copies bytes into your buffer - gigabytes of memcpy. Instead, mog
**maps** the file into the address space (`mmap`):
[Replay.hpp](../include/mog/Replay.hpp) asks for `MAP_POPULATE` (fault
every page into RAM up front, so replay timing never includes disk
stalls) and advises `MADV_SEQUENTIAL` (the kernel prefetches ahead). The
parser then reads message bytes straight out of shared kernel memory:
zero copies, zero syscalls during the run.

## 6. The Python shim: nanobind

Researchers live in Python; the engine lives in C++.
[_core.cpp](../python/mog/_core.cpp) bridges them with **nanobind** (a
lean modern successor to pybind11). The binding layer is deliberately a
*shim*: it converts Python arguments to C++ types and forwards calls -
no logic lives there, so there is nothing to fall out of sync with the
engine, and Python users get native speed because all work happens in
C++.
