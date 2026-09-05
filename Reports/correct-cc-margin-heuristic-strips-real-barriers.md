# `correct-cc` reports `CORRECTED` on a cubin that actually faults on real hardware

**Status:** open, root cause isolated to a specific mechanism (the new margin heuristic), exact
faulty computation not yet pinned down. Written for a fresh Claude Code session picking this up
cold. This is a **new, more serious finding than
`Reports/correct-cc-overconservative-barrier-intervals.md`** (that report's `Unrepairable`
false-positive is confirmed fixed by commits `3e51b87`/`1968ee9`/`15091a2` -- this is a
**regression introduced by that same fix**, specifically the "minimum stall count table" /
natural-spacing-margin heuristic those commits added to `collectBarrierIntervals()`).

**Why this is worse than the bug it fixed:** the old bug rejected a kernel that was actually fine
(safe, just annoying). This one does the opposite -- `correctControlCodes()` returns
`CheckStatus::Corrected` (its own `simulateAndVerify()` self-check passes) for a cubin that then
**genuinely crashes the GPU** (`CUDA_ERROR_ILLEGAL_ADDRESS`) when actually launched. A self-check
that passes on unsafe output means the model has a real blind spot, not just an overcautious one.

## Isolated repro (no SASS-Shuffler needed)

Same kernel as the other report: `Reports/gemm-tiled-repro.cu` / `.sass`.

```bash
nvcc -arch=sm_75 -cubin Reports/gemm-tiled-repro.cu -o /tmp/gemm.cubin

# Disassemble -> reassemble with ZERO control-code correction (CuAsmParser::parse +
# saveAsCubin only -- e.g. via CuAsmTools' own CubinFile/CuAsmParser API, no CorrectCC.hpp call).
# This "raw reassembly" cubin launches and runs correctly -- byte-identical control codes to the
# original ptxas output, confirming the disassemble/reassemble round-trip itself is faithful.

# Now run correct-cc on that raw reassembly:
build/bin/Release/correct-cc.exe raw_reassembly.cubin -o corrected.cubin -v
#   PROC - Kernel "gemm_tiled": CORRECTED -- Recomputed control codes for 88 instructions
#   (4/6 scoreboard slots used).

# Launch corrected.cubin (e.g. via SASS-Shuffler's KernelLauncher.hpp, or any Driver-API launcher):
#   cuEventSynchronize failed: CUDA_ERROR_ILLEGAL_ADDRESS (an illegal memory access was encountered)
```

Confirmed via a throwaway isolation test (not committed) that launched: (1) the original cubin --
fine; (2) a raw reassembly with **zero** correction applied -- fine, byte-identical control codes
to (1); (3) that same raw reassembly run through `correctCubinControlCodes()` -- **faults**. So
the fault is introduced specifically by correction, not by `CuAsmParser`'s reassembly itself, and
not by anything SASS-Shuffler does (no reorder was ever applied in this whole test).

## The exact evidence: a control-code diff

Dumping both cubins' control codes with `verify-cc.exe <cubin> -v` and diffing them shows exactly
what correction changed. Representative lines (`[address] waitbar:readbar:writebar:yield:stall`):

```
RAW (working):        CORRECTED (faults):
[0x2a0] W0:-:S08       [0x2a0] W-:-:S08
[0x2b0] W1:-:S04       [0x2b0] W-:-:S04
[0x2c0] W2:-:S04       [0x2c0] W-:-:S04
[0x2d0] W3:-:S04       [0x2d0] W-:-:S04
[0x2f0] W4:-:S08       [0x2f0] W-:-:S08
[0x300] W5:-:S04       [0x300] W-:-:S04
...  (also 0x310, 0x320, 0x350, 0x380, 0x3a0, 0x3b0, 0x3e0, 0x410, 0x430, 0x440 -- same pattern)
```

Every one of these is an `LDS.U`/`LDS.U.128` instruction (the shared-memory tile loads in
`gemm_tiled`'s inner `k`-loop) that **had a real, ptxas-assigned write-barrier slot in the working
schedule**, and correction **strips it entirely** (`writebar` field goes from a real slot number
to `7`/unset, i.e. "this producer needs no barrier"). Since `newCodes[i]`'s `writebar`/`readbar`
fields (`correctControlCodes()`, `bin/ccCommon.hpp`) are computed purely from
`writeBarOf[i]`/`readBarOf[i]` (which only get set for producers that ended up in
`collectBarrierIntervals()`'s output list), a producer the new margin heuristic decides to skip
gets its barrier unconditionally erased -- there is no "leave the original field alone if unsure"
fallback.

## Root cause: the margin heuristic, not the coloring or the stall-bump logic

`collectBarrierIntervals()` (commits `1968ee9`/`15091a2`, `bin/ccCommon.hpp`) now skips creating a
`BarrierInterval` for a producer whenever:

```cpp
const std::uint32_t distance = static_cast<std::uint32_t>(interval.firstConsumerIndex - producer - 1);
const std::uint32_t margin = distance + instrs[producer].ctrlCode.getStallCount();
if (margin >= minSafeBarrierMargin(instrs[producer].insKey)) {
    continue;  // treated as "safe by natural spacing alone" -- no interval, no barrier
}
```

`minSafeBarrierMargin("LDS_R_ARI")` is curated as a **flat constant, 13** -- calibrated from
exactly *one* witnessed instance in this same kernel (the `/*0290*/` `LDS.U`, whose real margin to
its consumer at `/*0330*/` is genuinely 13 and genuinely safe unbarriered -- that one instance is
*not* wrong, it's still correctly unbarriered in the diff above). The bug: this single measured
margin is being applied as a **blanket per-opcode threshold** to every other `LDS_R_ARI` instance
in the kernel too, several of which (the ones in the diff above) evidently have a *smaller* real
required margin -- ptxas itself assigned them a real barrier, which is direct, first-party
evidence they needed one -- yet whatever `distance`/`margin` this code computes for them comes out
`>= 13` anyway, wrongly clearing the bar.

**This needs one of two things to actually fix, and the report doesn't have enough further
digging to say which:**

1. **A computation bug**: `distance`/`margin` for these specific instructions might be getting
   computed wrong (e.g. `firstConsumerIndex` picking a farther-away consumer than the real nearest
   one, or `getStallCount()` reading the wrong instruction's stall) -- worth hand-verifying against
   `Reports/gemm-tiled-repro.sass`'s real addresses for at least one of the flagged instructions
   (e.g. the `LDS.U.128 R4, [R25]` at `/*02a0*/`: find its real nearest barrier-eligible consumer
   and hand-compute what distance+stall *should* be, then compare against what the code actually
   computes).
2. **The premise itself is unsound**: a flat per-opcode margin constant may just not generalize --
   the real safe margin for an `LDS_R_ARI` instance likely depends on the specific shared-memory
   bank/address pattern, warp occupancy, or other per-instance factors this model has no way to see,
   and one witnessed-safe instance doesn't bound every instance of the opcode.

## Suggested safety net (independent of which of the above is true)

Regardless of the margin computation's correctness, correction should never *erase* direct,
first-party evidence that a producer needs a barrier: **if a producer's own input control code
already has a barrier field set for this access (`getWriteSB()`/`getReadSB() != -1`), don't let
the margin heuristic un-set it** -- treat that as an automatic "needs an interval" regardless of
what the curated margin table says, the same "real evidence beats a guessed/curated constant"
principle `minSafeBarrierMargin()`'s own doc comment already claims to follow. This is a minimal,
low-risk change (a producer that already needed one still gets one; a producer whose input never
had one is unaffected either way) that would have caught this exact bug -- every one of the
flagged instructions in the diff above already had a real barrier in its input, and got it removed
anyway.

This wouldn't fix a *newly introduced* producer that's actually unsafe with margin skipped and had
no prior barrier to preserve (e.g. after SASS-Shuffler reorders something so a previously-safe gap
shrinks) -- that's the deeper margin-correctness question in the two options above -- but it would
stop correction from actively making a previously-safe input *less* safe, which is what's actually
happening here.

## Files referenced

- `bin/ccCommon.hpp`: `minSafeBarrierMargin()`, `collectBarrierIntervals()`,
  `correctControlCodes()`'s `writeBarOf`/`readBarOf`/`newCodes` construction.
- `Reports/gemm-tiled-repro.cu` / `Reports/gemm-tiled-repro.sass`: repro kernel and disassembly
  (shared with the other report).
- `Reports/correct-cc-overconservative-barrier-intervals.md`: the original bug this fix targeted,
  confirmed resolved -- this report is specifically about the regression the fix introduced.
