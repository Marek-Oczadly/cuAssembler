# `correct-cc` reports `Unrepairable` for a kernel that genuinely fits in 6 slots

**Status:** open, root cause identified with direct trace evidence, fix not yet attempted. Written
for a fresh Claude Code session picking this up cold. This is a **third, distinct bug**, uncovered
immediately after applying the fix for
`Reports/correct-cc-margin-heuristic-strips-real-barriers.md` (the uncommitted
`bin/ccCommon.hpp`/`Tests/test_ccCorrect.cpp` diff as of this writing: removed the disproven
`LDS_R_ARI` margin-13 table entry, and added `collectBarrierIntervals()`'s "never let the margin
heuristic un-set a barrier the input already had" safety net). That fix is correct as far as it
goes -- it stops correction from *erasing* real barriers -- but re-running the same repro kernel
through `correct-cc` now reports:

```
ERROR - Kernel "gemm_tiled": UNREPAIRABLE -- Cannot repair in place: 22 VARIABLE-latency
producer(s) need more scoreboard slots than the 6 physical slots available for this instruction
order; control codes were left unchanged.
```

`verify-cc` still confirms the **original, unmodified** cubin's control codes are 100% correct
(`Verification: PASSED -- All hazards across 88 instructions are correctly closed`). So a valid
6-slot assignment for these same 22 producers, in this same instruction order, provably exists --
ptxas already found it. `correct-cc`'s from-scratch search says it's impossible. That is a
contradiction, not a hardware limit being correctly enforced.

## The direct evidence: shared slots

Dumping the original cubin's control codes (`verify-cc.exe /tmp/gemm.cubin -v`) around the
`gemm_tiled` inner `k`-loop (`Reports/gemm-tiled-repro.sass` addresses `/*0290*/`-`/*0440*/`)
shows three **different** `LDS.U` producers all issuing to the **same** physical slot, back to
back, with **one shared wait** retiring all three:

```
[0x300] W5:-:S04   LDS.U R31, [R0.X4+0x540]   <- producer A, writebar slot 5
[0x310] W5:-:S04   LDS.U R32, [R0.X4+0x580]   <- producer B, writebar slot 5 (same slot, no wait in between)
[0x320] W5:-:S01   LDS.U R33, [R0.X4+0x5c0]   <- producer C, writebar slot 5 (same slot, no wait in between)
...
[0x3f0] B-----5:...  FFMA R9, R31, R9, R8      <- waits slot 5 once; retires A, B, and C together
```

(The same pattern repeats for slots 0-4 earlier in the same loop body -- see the full trace,
`/tmp/verify_full.txt` at time of writing, or re-run `verify-cc.exe -v` yourself.)

This is real, ptxas-emitted, hardware-valid code: Turing/Ampere's LDS-class scoreboards are
**counting/FIFO** barriers, not single-occupant locks -- multiple in-flight ops tagged with the
same slot id complete in issue order, so a single wait on that slot is sufficient to guarantee
*every* op tagged with it (not just the most recent one) has retired. ptxas exploits this
deliberately: three producers share one physical slot concurrently, closed by one shared wait,
instead of needing three distinct slots.

## Root cause: `assignBarrierSlots()`'s interval-coloring model has no notion of slot sharing

`assignBarrierSlots()` (`bin/ccCommon.hpp`) builds an interference graph where **any two
`BarrierInterval`s that overlap get an edge**, then colors it -- i.e. it hard-codes the assumption
that two overlapping producers must always use *different* slots, exactly like scheduling
non-shareable exclusive resources (e.g. registers). Producers A/B/C above all have intervals that
extend to the same shared retirement point (`0x3f0`), so their intervals mutually overlap by this
model's definition -- `assignBarrierSlots()` would demand 3 distinct colors for them alone, when
the real, working schedule uses exactly 1. Multiply that across every such group in this densely
unrolled loop and the apparent chromatic number blows past 6, even though the real, ptxas-valid
resource usage never does.

This is a fundamentally different problem from the previous two reports (which were both about
*whether* a producer needs an interval at all -- over-eager allocation, then over-eager margin-based
erasure). This one is about what happens *after* a producer is correctly identified as needing
protection: the coloring model's resource semantics (mutual exclusion) don't match the real
hardware's resource semantics (a counting/FIFO barrier that multiple producers can share, provided
they're waited on together and retire in issue order).

## Suggested fix direction (not yet attempted)

Before treating two overlapping `BarrierInterval`s as requiring distinct slots, check whether they
can instead be **merged into one group sharing a slot**: this is sound whenever the intervals'
`firstConsumerIndex` values coincide (or, more generally, whenever it's safe to delay every
producer's wait to the single latest `firstConsumerIndex` in the group, and their real completion
order is guaranteed FIFO -- true for same-opcode-class producers to the same scoreboard type
issued in program order, which `LatencyClassTable`'s classification already distinguishes). This
turns `assignBarrierSlots()` from a pure interval-graph coloring problem into a "coloring with
groupable/shareable intervals" problem -- closer to how ptxas itself schedules -- and would likely
need `collectBarrierIntervals()` to also widen a producer's effective retirement point to the
group's shared wait, not just its own individually-nearest consumer, mirroring what the trace above
shows ptxas actually doing.

**Caveat worth checking**: confirm the FIFO-completion assumption actually holds across *different*
LDS addressing-mode variants or mixed producer opcodes sharing a slot (this repro's example is all
plain `LDS.U`/`LDS.U.128` to shared memory) -- if two different scoreboard-eligible instruction
classes can be reordered relative to each other by the hardware even when tagged to the same slot,
grouping them would be unsound. `LatencyClassTable`'s existing curation is the place to check this.

## Files referenced

- `bin/ccCommon.hpp`: `BarrierInterval`, `collectBarrierIntervals()`, `assignBarrierSlots()`.
- `Reports/gemm-tiled-repro.cu` / `Reports/gemm-tiled-repro.sass`: repro kernel and disassembly
  (shared with the other two reports).
- `Reports/correct-cc-overconservative-barrier-intervals.md`,
  `Reports/correct-cc-margin-heuristic-strips-real-barriers.md`: the first two bugs found on this
  same kernel -- this is the third, found immediately after fixing the second.
