# `correct-cc` reports a real, verified-correct kernel as `Unrepairable`

**Status:** open, root cause identified, fix not yet attempted. Written for a fresh Claude Code
session picking this up cold -- no prior context assumed beyond this file and the repo itself.

**Where this came from:** discovered while using `SASS-Shuffler` (a sibling project, C++ RL tool
that reorders SASS instructions and uses `correct-cc`'s library entry point,
`CuAsmTools/CorrectCC.hpp`, to repair control codes after a reorder) against a real, moderately
complex GEMM kernel. The failure reproduces with **zero SASS-Shuffler involvement** -- see the
repro below, which only touches `correct-cc`/`verify-cc` directly.

## Summary

`correct-cc` (`bin/correct-cc.cpp` / `CuAsmTools/CorrectCC.hpp::correctCubinControlCodes()`)
recomputes a kernel's scoreboard control codes from scratch and reports whether a valid
assignment exists within the 6 physical scoreboard slots. Given a real, ptxas-compiled cubin
whose **existing** control codes `verify-cc` confirms are already 100% correct, `correct-cc`
reports the kernel as `CheckStatus::Unrepairable` -- claiming 28 simultaneously-live
VARIABLE-latency producers need scoreboard slots, when a real, working, verified-correct schedule
for the exact same instruction order already exists and uses far fewer.

Since `assignBarrierSlots()`'s interval-coloring algorithm is provably optimal for the graph it's
given (greedy coloring in increasing-start order is an exact algorithm for interval graphs, not a
heuristic -- see that function's own doc comment), the bug is not in the coloring step. It's in
what builds the input to that step: `collectBarrierIntervals()` (`bin/ccCommon.hpp`) creates a
full scoreboard-slot interval for **every** hazard edge from a VARIABLE-latency producer,
regardless of how much natural instruction spacing already separates the producer from its first
consumer -- it never considers that a real, working schedule (this kernel's own, confirmed by
`verify-cc`) can close such a hazard with *zero* barrier at all when the gap is already large
enough, the same "adjacent + zero-stall is the only unconditionally-wrong case" approximation
`simulateAndVerify()` already relies on elsewhere in this same file.

## Isolated repro (no SASS-Shuffler needed)

Kernel source: `Reports/gemm-tiled-repro.cu` (copied alongside this file) -- a tiled,
shared-memory GEMM kernel adapted from NVIDIA's `cuda-samples` `matrixMul` example, instantiated
at `BLOCK_SIZE=16`. Its full disassembly (`cuobjdump -sass`) as compiled below is saved at
`Reports/gemm-tiled-repro.sass`.

```bash
nvcc -arch=sm_75 -cubin Reports/gemm-tiled-repro.cu -o /tmp/gemm.cubin

# 1. The EXISTING (ptxas-generated) control codes are already fully correct:
build/bin/Release/verify-cc.exe /tmp/gemm.cubin -v
#   Verification: PASSED -- All hazards across 88 instructions are correctly closed by their
#   current control codes.

# 2. The SAME cubin, unmodified, fed to correct-cc:
build/bin/Release/correct-cc.exe /tmp/gemm.cubin -o /tmp/gemm_corrected.cubin -v
#   ERROR - Kernel "gemm_tiled": UNREPAIRABLE -- Cannot repair in place: 28 VARIABLE-latency
#   producer(s) need more scoreboard slots than the 6 physical slots available for this
#   instruction order; control codes were left unchanged.
```

Both re-run and reconfirmed directly against this repo's own `build/bin/Release/` binaries
(built via this repo's normal `build.bat`/CMake, current `master` @ `d196275`) -- identical output
to the FetchContent-pulled copy SASS-Shuffler pins (`c26a3ce`), so this isn't specific to either
commit.

**This is the whole bug in two commands.** No reorder, no mutation, no SASS-Shuffler round-trip
through `CuAsmParser` -- just the original compiler output, fed straight to both tools. One says
it's already correct; the other, computing from scratch, says a correct schedule is impossible.

## Root cause

Look at the loop body in `Reports/gemm-tiled-repro.sass` around `/*0290*/`-`/*0430*/` (the
`#pragma unroll`ed `k`-loop reading the shared-memory tiles): a long, densely packed run of
`LDS.U`/`LDS.U.128` instructions feeding `FFMA` accumulation, e.g.:

```
/*0290*/  LDS.U R16, [R0.X4+0x400] ;
/*02a0*/  LDS.U.128 R4, [R25] ;
/*02b0*/  LDS.U R17, [R0.X4+0x440] ;
...
/*0330*/  FFMA R4, R16, R4, R19 ;
...
```

None of these `LDS.U` instances set a write barrier at all (confirmed by inspecting their control
codes via `verify-cc -v`'s dump, or just by `verify-cc` accepting them with no violations) --
ptxas judged the natural instruction spacing between each load and its consuming `FFMA` already
sufficient, exactly the documented (if imprecise) model this codebase already uses for
FIXED-latency hazards elsewhere (`simulateAndVerify()`'s own doc, `bin/ccCommon.hpp`):

> "it only catches the unconditionally-wrong case (a true dependency made adjacent with zero
> stall between producer and consumer), and treats every other FIXED-latency ... edge as
> satisfied."

`collectBarrierIntervals()` (`bin/ccCommon.hpp`, ~line 1132) does not apply this same reasoning to
VARIABLE-latency producers. Its only test is `isBarrierEligible()` (~line 1095):

```cpp
inline bool isBarrierEligible(const LatencyClassEntry& producerLatency, BarrierType relevantAccess) {
    return producerLatency.kind == LatencyKind::VARIABLE && producerLatency.barrier.has_value() &&
           *producerLatency.barrier == relevantAccess;
}
```

This is purely a per-*opcode* classification check (LDS is curated `VARIABLE:WRITE` in
`LatencyClassTable`) -- it has no notion of "how far apart are the producer and its first real
consumer in this instruction order." Every edge that passes it gets a full
`BarrierInterval{producerIndex, firstConsumerIndex, direction}` (~line 1145), unconditionally,
even when `firstConsumerIndex` is already many instructions past `producerIndex` and a real,
already-verified-correct schedule proves no barrier was needed for that specific instance at all.
With this GEMM kernel's dozen-plus interleaved `LDS.U` producers all "needing" a reserved slot
simultaneously under this all-or-nothing rule, the interval-overlap count blows past 6 --
correctly, *given the (wrong) premise that every one of them needs an interval at all*.

## Suggested fix direction (not yet attempted)

Before `collectBarrierIntervals()` allocates an interval for a producer/edge pair, first check
whether the edge would already be safely closed under the exact same rule
`simulateAndVerify()`/`correctControlCodes()`'s own FIXED-latency fallback already relies on for
every *other* hazard type in this file: `!(consumer == producer + 1 && wouldBeZeroStall)`. If the
producer and its first barrier-eligible consumer are **not** immediately adjacent, this hazard can
be closed the same cheap way a FIXED-latency one already is here (bump `requiredStall[producer]`
to `1` if truly adjacent-with-zero-stall, otherwise leave it alone) -- no scoreboard interval
needed at all. Only fall back to allocating a real `BarrierInterval` (and therefore consuming one
of the 6 physical slots) when the producer/consumer pair is genuinely adjacent with no stall
margin, i.e. the one case this codebase's existing conservative model can't otherwise discharge.

This is not a new risk category: it's applying the *same* accepted approximation
(`simulateAndVerify()`'s own documented "conservative lower bound, not an exact cycle model")
this codebase already uses for FIXED-latency producers, to VARIABLE-latency producers with
sufficient natural spacing too, rather than inventing a stricter or laxer rule. It should not
regress any of `Tests/CheckControlCodes`'s existing fixtures (none of which needed >6 slots in the
first place, per the current test suite passing), and should fix this one by sharply reducing how
many of the 28 "needed" intervals actually require a real slot.

**Caveat worth checking before committing to this fix:** confirm this doesn't just move the bug --
if there's a *different* GEMM-style kernel (or a mutated one, from SASS-Shuffler's actual RL
reordering) where the natural spacing shrinks below one instruction for enough of these producers
that the interval count still exceeds 6, this fix reduces the false-positive rate without changing
the fundamental scoreboard-slot ceiling. That's expected and fine (6 physical slots is a real
hardware limit `assignBarrierSlots()` correctly enforces once given accurate intervals) -- just
don't expect this fix to make *every* possible kernel/reorder repairable, only to stop rejecting
ones that were never actually broken.

## Files referenced

- `bin/ccCommon.hpp`: `isBarrierEligible()`, `collectBarrierIntervals()`, `BarrierInterval`,
  `assignBarrierSlots()`, `correctControlCodes()`, `simulateAndVerify()` (for the FIXED-latency
  fallback rule to mirror).
- `CuAsmTools/CorrectCC.hpp`: `correctCubinControlCodes()`, the library entry point both
  `bin/correct-cc.cpp` and (externally) SASS-Shuffler call.
- `Reports/gemm-tiled-repro.cu` / `Reports/gemm-tiled-repro.sass`: the repro kernel and its real
  disassembly (both copied alongside this file).
