# `correct-cc`'s same-insKey slot sharing produces self-consistent but wrong control codes

**Status:** open, root cause identified with concrete evidence, fix not yet attempted. Written for
a fresh Claude Code session picking this up cold. This is a **fourth, distinct bug**, found
immediately after commit `4ea63c9` ("Fixed yet another issue") -- the fix for
`Reports/correct-cc-shared-slot-reuse-not-modeled.md` -- was applied.

**Where this came from:** SASS-Shuffler's `example/gemm/` training run against
`Reports/gemm-tiled-repro.cu`/`.sass` (same repro kernel as all three prior reports). With
`4ea63c9` built in, the run no longer crashes at `Episode::reset()` (the previous
`AssembleFailed`/`Unrepairable` failure is gone) -- but it now fails differently:

```
Episode::reset: the unmodified kernel itself failed verification/benchmarking (status TestFailed)
```

`TestFailed` (`SASS-Shuffler/RL/RewardEngine.hpp`) means the reassembled+corrected cubin **launched
and ran** but its output didn't match the golden `-o` data -- a real, silent wrong-answer bug, not
a crash and not a rejected reorder. This is on the **unmodified** baseline kernel (no reorder
applied at all), same class of problem the other three reports are about: `reassembleKernelProgram()`
(`SASS-Shuffler/RL/Reassemble.hpp`) unconditionally round-trips every kernel through
`CuAsm::Tools::correctCubinControlCodes()`, even when nothing was mutated.

## The evidence

Isolated repro, no SASS-Shuffler involved:

```bash
nvcc -arch=sm_75 -cubin Reports/gemm-tiled-repro.cu -o /tmp/gemm.cubin
build/bin/Release/correct-cc.exe /tmp/gemm.cubin -o /tmp/gemm_corrected.cubin -v
#   PROC - Kernel "gemm_tiled": CORRECTED -- Recomputed control codes for 88 instructions
#   (1/6 scoreboard slots used).
build/bin/Release/verify-cc.exe /tmp/gemm_corrected.cubin -v
#   Verification: PASSED -- All hazards across 88 instructions are correctly closed by their
#   current control codes.
```

**1 out of 6 scoreboard slots used is the red flag.** The *original*, ptxas-compiled,
verify-cc-confirmed-correct control codes for this exact kernel (see the other three reports) use
**all 6** physical slots concurrently in this same loop body (`W0` through `W5`, each holding
several producers via legitimate same-slot reuse). It is not plausible that a kernel whose real,
working schedule needs all 6 physical slots can be correctly served by just 1 -- and the training
harness's own real-hardware launch confirms it isn't: `TestFailed`.

## Root cause: "same insKey" is not the same as "safe to share"

Commit `4ea63c9`'s fix (`bin/ccCommon.hpp`, `BarrierInterval::insKey` + `assignBarrierSlots()`)
lets any two *overlapping* `BarrierInterval`s skip needing distinct slots whenever
`intervals[i].insKey == intervals[j].insKey` -- i.e. same opcode + operand shape. In this kernel,
essentially every barrier-needing producer in the inner loop is the same `LDS.U`/`LDS.U.128`
addressing-mode class, so this single rule collapses nearly the entire producer set into one
mutually-shareable group -- exactly matching the observed "1/6 slots used".

The fix's own doc comment argues this is safe because "an early retirement can never un-satisfy a
later real consumer" and each producer still gets its own individual wait bit at its own
`firstConsumerIndex`. That reasoning has a gap: it only accounts for what happens to *one*
producer's own wait. It does not establish what the *physical scoreboard hardware* does when many
producers are tagged to the *same* slot **concurrently** (i.e., several are simultaneously
in-flight, unretired, on one slot at once -- which is exactly what "overlapping intervals sharing a
slot" means). The comment's safety argument implicitly assumes the physical slot behaves as an
unbounded, fully-independent-per-producer FIFO counter that can track any number of concurrent
producers and correctly gate a wait on "have specifically the producers I depend on completed" --
but the only *confirmed* evidence in this repo (`Reports/gemm-tiled-repro.sass`) is a **narrow**
case: 2-3 back-to-back producers of the same class, sharing a slot, with the wait for that slot
placed *after all of them*. Generalizing that narrow, confirmed pattern into "any producer of this
opcode class anywhere in the interval graph can share a slot with any other overlapping one,
regardless of how many, regardless of where each one's own wait lands relative to the others" is
the unconfirmed leap, and the real hardware result (`TestFailed`) says it's wrong -- most likely
because either (a) the real scoreboard has a limited concurrent-tracking depth that this kernel's
~16-22-producer group exceeds, or (b) an early producer's wait firing when only *some* of the
group's earlier members have completed incorrectly reads the slot as fully clear (a single shared
"busy" flag semantics rather than a true per-producer-aware counter), letting a later consumer read
stale shared-memory data before its actual producer finished.

## Suggested fix direction (not yet attempted)

The shareability test needs to be tightened from "same insKey" to something closer to the one
concretely-confirmed-safe pattern: producers that are consecutive (or near-consecutive, with no
intervening barrier-eligible instruction of the *same* direction) in program order, sharing a slot
only with the immediately-adjacent chain they're issued alongside -- not transitively with every
other same-opcode producer anywhere in the interval graph regardless of distance or group size.
Concretely: cap how many producers can be merged into one shareable group (calibrate against
`Reports/gemm-tiled-repro.sass`'s own confirmed-safe 2-3-producer groups -- don't assume larger
groups are safe without new real evidence, the same "only curate what's confirmed" policy
`minSafeBarrierMargin()` already claims to follow), and/or require the merged group's producers to
be contiguous in program order (no other producer of a different class or direction issued in
between) rather than merely overlapping-and-same-opcode anywhere in the kernel.

**However this is fixed, the fix needs an empirical check this repo doesn't have yet**: none of
the confirming evidence for *any* of these four reports comes from actually launching a corrected
cubin and checking its output on real hardware -- `verify-cc`'s self-check only confirms internal
consistency against this codebase's own (evidently incomplete) hazard model, not real GPU
correctness. `Tests/CheckControlCodes`' fixtures are all cubins that were never found to need more
than a few slots in the first place, so none of them would have caught this. Consider adding a
must-launch-and-compare-real-output fixture from this same GEMM kernel (or similar
barrier-slot-pressure test) to this repo's own test suite so a future slot-sharing change can be
checked against ground truth directly, not just against this codebase's own model of ground truth.

## Files referenced

- `bin/ccCommon.hpp`: `BarrierInterval::insKey`, `collectBarrierIntervals()`, `assignBarrierSlots()`
  (the `shareable` check added in `4ea63c9`).
- `Reports/gemm-tiled-repro.cu` / `Reports/gemm-tiled-repro.sass`: repro kernel and disassembly
  (shared with the other three reports).
- `Reports/correct-cc-shared-slot-reuse-not-modeled.md`: the report this commit fixed -- this is
  the bug that fix introduced.
