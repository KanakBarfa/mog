# Chapter 05: The Price Ladder and the Order Hash Table

**What you will learn:** the two lookup structures at mog's core - how it
finds a price level instantly, and how it finds any single order among
millions instantly.

---

## 1. The big picture: skip the search, compute the answer

The book (chapter 01) answers two questions millions of times per second:

1. "What is waiting at price $100.05?" - needs **price to level**.
2. "Where is order #8817234 now?" - needs **order ID to slot**.

The textbook answer for both is a search tree (`std::map`), which walks
O(log N) nodes chasing pointers - each hop a possible cache miss. mog
instead *computes* both answers with one arithmetic operation each: O(1),
no searching at all.

## 2. Price to level: the paged dense ladder

Prices are already integers (ticks of $0.0001, chapter 01). Integers can
index arrays directly. But an array spanning every tick from $0.01 to
$1,200 would be absurdly large while mostly empty.

mog's compromise ([Ladder.hpp](../include/mog/Ladder.hpp)) works like a
hotel with 1,024-room floors: rooms are numbered continuously within a
floor, but floors are built only when a guest arrives needing them.

- `page = (tick - lowest_tick) / 1024` selects the floor;
- `offset = (tick - lowest_tick) % 1024` selects the room;
- in code both are bit operations (`>>` and `&`) because 1024 is a power
  of two - division becomes a shift.

A small directory maps page numbers to memory claimed on demand from a
fixed pool. Dense prices hit pure array indexing; arbitrarily wide spreads
never blow up memory; empty price ranges cost only a null entry in the
directory.

## 3. Order ID to slot: Fibonacci hashing

Order reference numbers arrive from the exchange effectively sequential
(8817234, 8817235...). Sequential numbers hashed naively pile into
adjacent slots - clustering is the enemy of hash tables.

The classic fix multiplies by a magic constant:

$$\text{slot} = (\text{ref} \times \texttt{0x9E3779B97F4A7C15}) \gg \text{shift}$$

In words: multiply the ID by a carefully chosen odd 64-bit number (the
integer approximation of the golden ratio), keep the high bits, use those
as the slot index. Multiplication scrambles sequential patterns so
neighbors land far apart; taking the top bits spreads them evenly across
the table. No modulo, no mixing rounds - one multiply and one shift,
about a nanosecond.

The table itself uses **open addressing**: if your slot is occupied, probe
onward at fixed offsets until a free one appears - everything stays in one
flat array that CPUs cache well. Find it wired into the book's order
lookup in [OrderBook.hpp](../include/mog/OrderBook.hpp).

## 4. Why this matters beyond speed

Both structures preserve determinism by construction: index math depends
only on input values, never on iteration order, pointer addresses, or
timing. The same feed produces the same slots in the same state on every
machine - which is exactly what chapter 00 promised.
