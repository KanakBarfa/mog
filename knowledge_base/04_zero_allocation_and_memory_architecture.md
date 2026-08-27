# Chapter 04: Zero-Allocation Memory Architecture

**What you will learn:** why mog never asks the operating system for
memory during a replay, and the three techniques that make that possible.

---

## 1. The big picture: set the tables before service starts

When a restaurant asks the kitchen to build a new table for every arriving
guest, service stalls unpredictably - some tables take seconds, some
collapse. Programs do this with **dynamic allocation** (`new`, `malloc`):
requesting memory from the OS mid-run. Each request can take microseconds,
occasionally milliseconds, and the timing varies with system state. For a
simulator replaying hundreds of millions of events, that is (a) slow and
(b) fatal to determinism: allocation timing can leak into behavior.

mog's rule: during replay, allocate **zero bytes**. All memory exists
before the first event. Three techniques make this workable.

## 2. Technique one: the AllocGuard tripwire

Rules nobody tests are wishes.
[AllocGuard.hpp](../include/mog/AllocGuard.hpp) replaces the global
allocation functions in debug builds with a version that aborts the
process if any allocation happens while the guard is armed. If a code
change sneaks an allocation into the hot path, CI dies immediately with a
stack trace pointing at the culprit. Zero-allocation stays enforced, not
aspirational.

## 3. Technique two: the arena - rent the whole floor

Instead of many small requests, mog takes one large contiguous block of
memory up front - an **arena** ([Arena.hpp](../include/mog/Arena.hpp)) -
and hands out numbered slots from it, like a hotel pre-divided into rooms.
Acquiring order storage means taking the next free room number; releasing
means putting the number back on a free list. Both are a few CPU cycles.

Two refinements matter:

- **Cache-line alignment**: CPUs move memory in 64-byte lines. Data
  spanning two lines costs two fetches; contended data sharing one line
  makes cores gossip over every write. Arena slots are aligned so each
  order node sits cleanly (`alignas(64)`).
- **Generational handles** (`GenHandle`): other components reference
  orders not by raw pointer but by (slot index, generation) pairs. When a
  slot is freed and reused, its generation counter increments. An old
  handle now carries a stale generation and fails safely instead of
  silently touching the new tenant. This kills the classic **ABA bug**:
  "the slot holds what I left there" being true about the wrong occupant.

## 4. Technique three: intrusive linked lists

Each price level's FIFO queue (chapter 01) is a linked list. Textbook
linked lists allocate a node per element - forbidden here. Instead the
order structs themselves hold their neighbors' slot indices:

```cpp
struct Order {
    std::uint64_t order_ref = 0;      // the exchange's ID for this order
    std::int64_t  qty_remaining = 0;  // shares still waiting
    std::int64_t  price_ticks = 0;
    std::uint32_t prev = kNullIndex;  // neighbor toward queue front
    std::uint32_t next = kNullIndex;  // neighbor toward queue back
    char side = 'B';                  // buy or sell
};
```

"Intrusive" means the linkage lives *inside* the data rather than in
separate wrapper nodes. Joining or leaving a queue rewires two integers;
no memory is touched beyond the orders themselves. See
[OrderBook.hpp](../include/mog/OrderBook.hpp).

The result: inserting, cancelling, or executing any order is a handful of
integer operations at constant cost - O(1) - with no allocator in sight.
