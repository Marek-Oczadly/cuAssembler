# `insKey` strips width/type modifiers, so slot-sharing still merges non-shareable producers

**Status:** open, root cause identified with concrete evidence, fix not yet attempted. Written for
a fresh Claude Code session picking this up cold. This is a **fifth, distinct bug**, found
immediately after commit `afc1b73` ("Fixed an issue") -- the contiguity/group-size-cap fix for
`Reports/correct-cc-insKey-sharing-too-broad.md` -- was applied.

**Where this came from:** re-running SASS-Shuffler's `example/gemm/` training harness against
`Reports/gemm-tiled-repro.cu` (identical source to `example/gemm/gemm_kernel.cu`) with `afc1b73`
built in. The previous `1/6 scoreboard slots used` collapse is gone -- `correct-cc` now reports a
plausible `5/6 scoreboard slots used` and `verify-cc` confirms PASSED -- but the training harness
still fails at `Episode::reset()`, on the **unmodified** baseline kernel, with the same class of
error as report 4:

```
Episode::reset: the unmodified kernel itself failed verification/benchmarking (status TestFailed) -- nothing to normalize reward against
```

So `afc1b73`'s fix is an improvement (it's no longer *wildly* wrong, and it's internally
self-consistent per `verify-cc`) but it is still **actually wrong** on real hardware. This confirms
report 4's closing caveat: `verify-cc`'s self-check only proves the corrected control codes are
internally consistent with this codebase's own hazard model -- not that they match real hardware
behavior.

## The evidence

Isolated repro (no SASS-Shuffler involved), using the `afc1b73` build:

```bash
nvcc -arch=sm_75 -cubin Reports/gemm-tiled-repro.cu -o /tmp/gemm.cubin
build/bin/Release/correct-cc.exe /tmp/gemm.cubin -o /tmp/gemm_corrected.cubin -v
#   CORRECTED -- Recomputed control codes for 88 instructions (5/6 scoreboard slots used).
build/bin/Release/verify-cc.exe /tmp/gemm_corrected.cubin -v
#   Verification: PASSED -- All hazards ... correctly closed.
```

Diffing `verify-cc -v`'s full control-code dump for the **original** cubin against the
**corrected** one (both from the same `afc1b73` build) shows real divergences, not just cosmetic
slot-numbering differences. The clearest one, around `Reports/gemm-tiled-repro.sass`'s
`/*02f0*/`-`/*0320*/`:

```
                         original    corrected
  [0x2f0]  LDS.U.128 R8, ...   W4          W1
  [0x300]  LDS.U R31, ...      W5          W1   <- merged with 0x2f0 in the corrected version
  [0x310]  LDS.U R32, ...      W5          W2
  [0x320]  LDS.U R33, ...      W5          W2
```

ptxas's real, verify-cc-confirmed-correct schedule keeps the `LDS.U.128` producer at `0x2f0` on its
**own** slot (`W4`), separate from the three plain `LDS.U` producers that follow it (`W5`, shared
per the pattern documented in `Reports/correct-cc-shared-slot-reuse-not-modeled.md`). The
`afc1b73`-corrected schedule instead merges `0x2f0` into the same share group as `0x300`, which is
a real, concrete deviation from the only confirmed-correct reference this repo has for this kernel
-- and the real-hardware `TestFailed` result says that deviation actually breaks correctness at
runtime.

## Root cause: `insKey` throws away the modifier that actually matters here

`CuInsParser::parse()` (`CuAsm/CuInsParser.cpp:252-297`) builds `insKey` like this:

```cpp
m_InsOpFull = m.str(2);                                    // e.g. "LDS.U.128" or "LDS.U"
const std::vector<std::string> opTokens = splitChar(m_InsOpFull, '.');
m_InsKey = opTokens[0];                                     // <- keeps only "LDS", drops ".U"/".128"
m_InsOp = opTokens[0];
...
for (const std::string& tok : opTokens) {
    m_InsModifier.push_back("0_" + tok);                    // ".U"/".128" go here instead
}
...
m_InsKey += "_" + op.type;                                  // append per-operand type tokens only
```

So `LDS.U R31, [R0.X4+0x540]` and `LDS.U.128 R8, [R25+0x10]` produce the **same** `insKey` (both
reduce to `"LDS_R_..."` -- bare mnemonic plus operand-*type* tokens; the destination operand's type
token is just `"R"` regardless of whether it names a single register or a 128-bit register quad).
The `.U`/`.128` distinction lives only in `m_InsModifier`/`insModi`, a separate return value that
`bin/ccCommon.hpp`'s `collectBarrierIntervals()` never captures onto `BarrierInterval` (only
`insKey` is stored there, per `BarrierInterval::insKey`, added in `4ea63c9` and unchanged in
`afc1b73`).

`computeShareGroups()` (`bin/ccCommon.hpp`, added in `afc1b73`) groups purely on
`intervals[i].insKey == intervals[i-1].insKey` (plus direction and contiguity/cap). Since `insKey`
can't distinguish a 32-bit load from a 128-bit load of the same base opcode, two producers that
are *not* actually interchangeable -- different width, different real latency/footprint, and
evidently not slot-shareable per ptxas's own choices -- get treated as the same shareable class.

This is a **different bug from report 4**, not a re-manifestation of it: report 4's fix (cap group
size at 3, require contiguity) is a reasonable tightening and is not itself shown wrong here -- what's
wrong is the granularity of the key being grouped on. Even a correctly-capped, correctly-contiguous
group is unsound if its members aren't actually the same instruction class to begin with.

## Suggested fix direction (not yet attempted)

`collectBarrierIntervals()` needs to capture enough of the producer's real instruction identity to
distinguish width/type variants before `computeShareGroups()` groups on it -- not just the
bare-mnemonic-plus-operand-type `insKey`. Concretely:

- Thread `instrs[producer].insModi` (or the subset of it that isn't purely a stall/predicate
  artifact -- worth checking exactly what ends up in `insModi` for a representative sample of
  opcodes) into a new `BarrierInterval` field, and require it to match (not just `insKey`) for two
  intervals to be shareable in `computeShareGroups()`.
- Simpler alternative given this repo's "only curate what's confirmed" policy: build the
  share-group key from `m_InsOpFull` (the *undecomposed* opcode text, e.g. `"LDS.U.128"` vs
  `"LDS.U"`) plus operand-type shape, rather than the already-lossy `insKey`. This avoids having to
  reason about which specific modifiers matter and which don't -- two producers only share a group
  if their full opcode text (modifiers included) is identical, which is at least as conservative as
  ptxas's own observed behavior and provably wouldn't have produced this bug's merge.
- Either way, re-verify against `Reports/gemm-tiled-repro.sass` after the fix: `0x2f0` must land on
  a distinct slot from `0x300`/`0x310`/`0x320`, and the `0x300`-group's shared-slot behavior
  (confirmed safe) must be preserved.
- **Still true from report 4, worth repeating**: this repo has no automated real-hardware
  launch-and-compare test. Every one of these five bugs was only caught because SASS-Shuffler's RL
  training harness happens to launch the corrected cubin on real hardware and check output --
  `verify-cc`'s self-check has now twice in a row (`4ea63c9`, `afc1b73`) reported PASSED on a cubin
  that was actually wrong. Consider adding a must-launch-and-compare fixture from this GEMM kernel
  (or a smaller kernel exercising mixed-width same-opcode producers sharing overlapping intervals)
  directly to this repo's own test suite, so this class of regression stops depending on an
  external project's training run to surface it.

## Files referenced

- `CuAsm/CuInsParser.cpp:252-297`: `CuInsParser::parse()`, where `insKey` is built and modifiers
  are discarded into `insModi` instead.
- `bin/ccCommon.hpp`: `BarrierInterval::insKey`, `collectBarrierIntervals()`, `computeShareGroups()`,
  `assignBarrierSlots()`.
- `Reports/gemm-tiled-repro.cu` / `Reports/gemm-tiled-repro.sass`: repro kernel and disassembly
  (shared with all prior reports).
- `Reports/correct-cc-insKey-sharing-too-broad.md`: the report `afc1b73` fixed -- this is a
  different bug found immediately after, in the same fixed area of code.
