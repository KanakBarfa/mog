# Chapter 06: SIMD, SWAR & Runtime ISA Dispatch

**What you will learn:** how mog decodes eight bytes per instruction, and
how the same binary stays correct and fast on CPUs from a decade apart.

---

## 1. The big picture: scan whole shelves, not single items

A cashier scanning one item per beep is how a CPU normally works: one
instruction, one number. Modern CPUs can instead act like a barcode
scanner passing over a whole shelf at once - **SIMD** ("Single
Instruction, Multiple Data") applies one operation to 8, 16, or 32 bytes
simultaneously. Parsing network data is mostly "reverse these bytes,
extract those fields" - embarrassingly parallel, ideal for SIMD.

## 2. SWAR: SIMD with ordinary registers

True SIMD needs special wide registers and per-CPU instruction sets.
**SWAR** ("SIMD Within A Register") gets part of the benefit using the
plain 64-bit register every CPU already has: treat it as eight
independent byte lanes.

The ITCH decoder's hot path ([SWAR.hpp](../include/mog/simd/SWAR.hpp))
loads 8 bytes of a message in one instruction, then byte-reverses them
with a compiler built-in (`__builtin_bswap64`) that compiles to the CPU's
single-cycle swap instruction (chapter 03 explained why reversal is
needed: network byte order). Eight bytes move and flip for the price of
about two instructions total.

## 3. Runtime ISA dispatch: one binary, many CPUs

Vector width differs across machines: baseline x86_64 guarantees SSE4;
most desktops add AVX2; servers often have AVX-512. Code compiled *for*
AVX-512 crashes on machines without it.

mog solves this with **runtime dispatch**
([IsaDispatch.hpp](../include/mog/IsaDispatch.hpp)):

1. At startup, query the CPU (`cpuid`) for its feature list.
2. Select the parsing kernel once: AVX-512 where available, else AVX2,
   else the portable SWAR baseline.
3. Every call afterwards goes through the chosen function pointer - the
   check happens once, not per message.

The safety net: all kernels decode identically. Differential tests feed
the same messages through every kernel and require bit-identical outputs
(doctrine #4, chapter 00) - speed must never come with behavior changes.

## 4. The discipline that keeps it honest

Hand-tuned code is allowed only when it proves itself: a benchmarked win
of at least 15% over both compiler output and the `std::simd` version,
plus a plain-C++ twin kept bit-identical by tests. Kernels failing that
bar are deleted - including one assembly attempt that lost to its own C++
twin and was removed, which the project records as a result rather than
hiding it.
