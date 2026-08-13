# Test Report — 2026-08-13

Source: `test-results.log` (fresh build, full CTest run)

**Initial result: 145/156 passed, 11 failed.**
**After fixing Reason 1 below: 149/156 passed, 7 failed.**

## Fix applied: Reason 1 (unresolved `vprintf`)

`CuAsmParser::Impl::evalVar` (in `cuAsm-CPP/CuAsm/CuAsmParser.cpp`) threw `Unknown expression <var>` for any identifier that wasn't a local label, even when that identifier was a legitimate external/undefined symbol already present in `.symtab` (e.g. `vprintf`, referenced by CUDA's device-side `printf`). It now falls back to returning `{0, true}` for symtab-only symbols, matching the pattern already used elsewhere in the file (see `evalInstructionFixup`'s backtick-label handling) so that `evalFixups` takes its existing relocation-emitting branch instead of erroring out.

This fixed 4 of the 6 `DevicePrintf` failures outright (sm_70, sm_75, sm_80, sm_86 now pass). The remaining 2 (sm_60, sm_61) now fail with **different, previously-hidden** errors — see below.

## Individual failures (current: 7 remaining)

1. **test_CheckDisasm_ExtendedAtomicOps_sm_75** — unchanged, unrelated to the fix. Assembling `RED.E.XOR.STRONG.GPU [R8], R25` fails in `NewModi` with `Unknown modifiers: ({'0_XOR'})`. The `.XOR` reduction-op modifier has no matching entry in the modifier table for this opcode on sm_75.

2. **test_CheckDisasm_DevicePrintf_sm_60** — *(newly exposed)* Now gets all the way through parsing and re-assembly, but the re-assembled cubin differs from the original at byte offset `0x428`: expected `0x8`, got `0x0`. This was previously masked by the `vprintf` fixup error, which aborted the test before reaching this comparison. Likely a leftover discrepancy in how the new `vprintf` relocation (or the section/symbol data it touches) is emitted for sm_60 specifically — needs its own investigation.

3. **test_CheckDisasm_DevicePrintf_sm_61** — *(newly exposed)* Assembling `JMX R0` now fails in `NewVals` with `Insufficient basis, try CuAsming more instructions!` (the assembler's "Known Records" for `JMX` only has an example with `R2`, not `R0`). Also previously masked by the `vprintf` error. This is the same class of bug as Reason 2 below (a lookup/basis table missing an entry) but for a different instruction (`JMX`) — not fixed here.

4. **test_CheckDisasm_CooperativeGroups_sm_70** — unchanged. Binary diff at byte offset `0x7a0`: expected `0x0`, got `0x1`.

5. **test_CheckDisasm_CooperativeGroups_sm_75** — unchanged. `LD.E.STRONG.GPU R2, [UR4+0x4]` fails in `NewModi` with `Unknown modifiers: ({'0_GPU'})`.

6. **test_CheckDisasm_CooperativeGroups_sm_80** — unchanged. Binary diff at byte offset `0x790`: expected `0x0`, got `0x1`.

7. **test_CheckDisasm_CooperativeGroups_sm_86** — unchanged. Binary diff at byte offset `0x790`: expected `0x0`, got `0x1`.

## Reasons

### Reason 1: Unresolved external symbol `vprintf` in fixup evaluation — FIXED

Was hit identically by all six `DevicePrintf` SM variants; see "Fix applied" above. Fixing it did not make those tests pass automatically for sm_60/sm_61 — it just let them progress far enough to hit two other, previously-hidden bugs (#2 and #3 above), which are unrelated to symbol resolution.

### Reason 2: Instruction modifier/basis table missing an entry for a given SM

**Affects:** #1 (`ExtendedAtomicOps` sm_75), #5 (`CooperativeGroups` sm_75), and now also #3 (`DevicePrintf` sm_61, a related-but-distinct `NewVals`/"Insufficient basis" variant of the same underlying class of bug)

All three fail while the assembler tries to match an instruction against a per-opcode lookup table (`NewModi` for modifiers, `NewVals` for operand "basis") built from known-good example instructions, and none of the tables has an entry covering the specific combination seen:

- `RED.E.XOR.STRONG.GPU` — table has `ADD`/`MAX`/`MIN`/`OR`/`AND` reduction ops for `RED.E`, but no `XOR`.
- `LD.E.STRONG.GPU` — table has `.SYS`/`.CTA` scope modifiers for `LD.E`, but no `.GPU`.
- `JMX R0` — table only has an example `JMX R2`, insufficient to derive the general encoding needed for `JMX R0`.

This points to gaps in the per-SM instruction definition data rather than a bug in the matching logic itself, since the logic clearly handles sibling modifiers/operands correctly elsewhere.

### Reason 3: Single-bit cubin mismatch on round-trip of kernels containing weak symbols

**Affects:** #4, #6, #7 (`CooperativeGroups` sm_70, sm_80, sm_86)

These three all get all the way through parsing and re-assembly, and fail only at the final binary-comparison step, with the new cubin differing from the original by exactly one bit (`0x0` vs `0x1`) at a late file offset (`0x7a0` for sm_70, `0x790` for sm_80/sm_86). All three (and also the sm_75 variant, which fails earlier for the unrelated modifier reason above) trigger a `preScan` warning: `Weak symbol found! The implementation is not complete, please be cautious...`. This warning is a direct admission in the code that weak-symbol handling is unfinished. The single flipped bit is consistent with one specific flag tied to weak-symbol/cooperative-kernel handling not being reproduced identically during re-assembly. For sm_80/sm_86 the pipeline also runs an extra "Demoting default-UR desc bits" pass during `saveAsCubin` (part of the SM_80+ descriptor-hack workaround), but since sm_70 shows the identical single-bit-diff symptom without that pass, the demoting step is not the root cause — the underlying incomplete weak-symbol handling is the common thread across all three.

### Reason 4 (new): `DevicePrintf` sm_60 binary mismatch after `vprintf` relocation fix

**Affects:** #2 (`DevicePrintf` sm_60)

Not yet root-caused. Parsing and re-assembly both succeed, but the resulting cubin differs from nvcc's original at byte offset `0x428` (expected `0x8`, got `0x0`). Since this only appeared after fixing Reason 1, it's plausibly related to how the new `R_CUDA_32` relocation against `vprintf` interacts with sm_60's layout (e.g. an addend, alignment, or duplicate-relocation edge case that only sm_60 hits), but this needs separate investigation and was out of scope for the Reason 1 fix.
