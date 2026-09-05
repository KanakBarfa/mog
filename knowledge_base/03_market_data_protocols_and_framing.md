# Chapter 03: Market Data Protocols & Wire Framing

**What you will learn:** how an exchange ships millions of events per day
as raw binary, what those bytes look like, and the two framing layers
mog understands.

---

## 1. The big picture: postcards vs bubble sheets

Describing one order in text ("BUY 100 NVDA @ 100.0500") costs dozens of
bytes plus parsing time. An exchange instead sends a **bubble sheet**: a
fixed-layout form where every field always sits at the same byte offset,
in a fixed total size. No labels, no separators - position IS meaning.
That is why NASDAQ's ITCH feed messages are 19 to 40 bytes each, and why
decoding them can be nearly free.

## 2. ITCH 5.0 message anatomy

Every message starts with a one-byte type character, then packed fields
([ITCHParser.hpp](../include/mog/ITCHParser.hpp)). The workhorse is Add
Order (`A`), 36 bytes:

```
offset  size  field
0       1     'A'                     message type
1       2     stock locate            which instrument (an integer index)
3       2     tracking number
5       6     timestamp               nanoseconds since midnight (48 bits)
11      8     order reference         the L3 order's unique ID
19      1     buy/sell flag
20      4     shares
22      8     stock symbol            e.g. "NVDA    " (padded to 8 chars)
30      4     price                   integer ticks of $0.0001 (see ch 01)
```

Other types follow the same idea: `F` (add with member-firm attribution,
40 bytes), `E` (execution, 31), `C` (execution at off-display price, 36),
`X` (partial cancel, 23), `D` (delete, 19 - just a reference number),
`U` (replace, 35). Session-level types: `S` system event, `H` trading
action (halts), `Q` cross trade, `P` trade print, `I` NOII imbalance.

Notice the 48-bit timestamp - not a friendly multiple of 8. Binary
formats pack tight; decoders must handle odd sizes.

## 3. Endianness: which end of a number comes first

Humans write 250 as "two-five-zero": most significant digit first. Network
protocols agreed to do the same (**big-endian**). Most CPUs - x86 laptops,
ARM phones - store numbers the opposite way, least significant byte first
(**little-endian**) [1].

So every multi-byte field needs its bytes reversed on arrival. CPUs do
this in a single instruction (`bswap`). The subtlety: ITCH fields start at
odd offsets (byte 5, byte 11...), while CPUs prefer loading numbers from
"aligned" addresses. Misaligned loads are undefined behavior in C++ unless
done carefully; mog uses `memcpy`, which every modern compiler lowers to
one plain load instruction ([Wire.hpp](../include/mog/Wire.hpp)).

[1] The names come from *Gulliver's Travels*, where nations warred over
which end of an egg to crack. Programmers kept the joke.

## 4. Two layers of envelopes

Raw ITCH messages travel inside two envelope formats:

- **BinaryFILE** (.itch files): the public download format. Each message
  is prefixed with a 2-byte length, so a decoder reads length then payload,
  repeatedly. Self-delimiting - even a partially downloaded file parses up
  to its last complete message.
- **MoldUDP64** ([MoldUdp.hpp](../include/mog/MoldUdp.hpp)): the live
  network protocol. Packets carry a 10-byte session ID, an 8-byte sequence
  number, and a count of contained messages. Because messages are numbered,
  a receiver can detect gaps instantly ("I got 41..50 but last time ended
  at 40 - wait, I skipped nothing; or did I miss 31..35?") and request
  retransmission. mog validates sequence continuity. A new session ID
  restarts sequencing even when it arrives on a heartbeat first (exchange
  failover), rather than misreading it as a gap.

Together: BinaryFILE for recorded history, MoldUDP64 for live wire - same
ITCH payloads inside.
