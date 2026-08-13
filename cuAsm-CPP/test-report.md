# Test Report — 2026-08-13 (09:13 BST)

Source: `test-results.log` (fresh build, full CTest run)

**Initial result: 145/156 passed, 11 failed.**
**After fixing Reason 1: 149/156 passed, 7 failed.**
**After fixing Reason 4: 150/156 passed, 6 failed.**
**After fixing Reason 2: 152/156 passed, 4 failed.**

## Fix applied: Reason 1 (unresolved `vprintf`)

`CuAsmParser::Impl::evalVar` (in `cuAsm-CPP/CuAsm/CuAsmParser.cpp`) threw `Unknown expression <var>` for any identifier that wasn't a local label, even when that identifier was a legitimate external/undefined symbol already present in `.symtab` (e.g. `vprintf`, referenced by CUDA's device-side `printf`). It now falls back to returning `{0, true}` for symtab-only symbols, matching the pattern already used elsewhere in the file (see `evalInstructionFixup`'s backtick-label handling) so that `evalFixups` takes its existing relocation-emitting branch instead of erroring out.

This fixed 4 of the 6 `DevicePrintf` failures outright (sm_70, sm_75, sm_80, sm_86 now pass). The remaining 2 (sm_60, sm_61) failed with **different, previously-hidden** errors, documented as Reason 4 below.

## Fix applied: Reason 4 (two stacked bugs in `DevicePrintf` sm_60)

Fixing Reason 1 let `DevicePrintf` sm_60 progress far enough to hit two more previously-hidden, unrelated bugs, both in `cuAsm-CPP/CuAsm/CuAsmParser.cpp`:

1. **Wrong label offset for JMX/BRX indirect-call sources.** `EIATTR_INDIRECT_BRANCH_TARGETS` records a `label@srel` reference to the *address of the branching instruction itself* (`evalVar`'s `@srel` handling just returns the label's raw recorded offset). Label offsets are normally recorded from the raw section write-cursor (`tellLocal()`) during `preScan`, which is correct almost everywhere — except on sm_5x/6x, where an 8-byte control word is interleaved before every 4th instruction slot. A label sitting on such a boundary (as `.L_x_2`, immediately before the `JMX R0` device-function-call dispatch, did here) gets recorded 8 bytes short of the instruction's real address. This exact control-word-aware-offset problem was already solved for one specific case — labels immediately preceding a `SYNC`/`BRK` reconvergence instruction, via `labelPrecedesSyncOrBrk` — but `JMX`/`BRX` (the sm_5x/6x indirect-call/branch instructions used for calling device functions like `vprintf` through a function pointer) weren't covered by that check, so their source-label offsets stayed uncorrected. Broadened the lookahead (renamed to `labelPrecedesIndirectBranchSource`) to also recognize `JMX`/`BRX`, matching the pattern already confirmed correct for `SYNC`/`BRK` and cross-checked against the structurally-identical `BRX`-based case in `TestData/CuTest/cudatest.6.sm_61.cuasm`.

2. **`.align` inside a section body could clobber the section's own `sh_addralign`.** `CuAsmSection::emitAlign` treats any `.align N` directive seen while the write cursor is still at position 0 as *setting the section's header alignment* (`sh_addralign`), not just padding — correct for the very first `.align` in a section (which is exactly how the disassembler encodes the section header's own declared alignment), but wrong for any *later* `.align` directive that also happens to land at position 0 because nothing has been emitted yet (e.g. a smaller alignment requirement for the section's first data item, like the 4-byte `.word vprintf` in `.nv.constant4`, following the header's own `.align 8`). Every subsequent zero-position `.align` was overwriting the header value, silently corrupting `sh_addralign` whenever the first content item needed a smaller alignment than the section as a whole. Fixed by only honoring the *first* zero-position `.align` call per section (guarded by `addralign == 0`, the section's un-set default) — later ones now correctly act as pure no-ops, since there's nothing to pad at position 0 anyway.

Verified via full 156-test suite: no regressions, `DevicePrintf` sm_60 now passes byte-for-byte.

## Fix applied: Reason 2 (instruction modifier/basis table gaps)

Unlike Reasons 1/4, this wasn't a code bug: `CuInsAssembler`/`CuInsAssemblerRepos` (`cuAsm-CPP/CuAsm/CuInsAssembler.cpp`, `CuInsAssemblerRepos.cpp`) derive an instruction's binary encoding from a linear system built from previously-recorded real examples per opcode shape (`InsKey`). A modifier the assembler has never seen for that `InsKey` throws `NewModi`; a modifier combination it *has* seen individually but not in this exact value combination throws `NewVals` ("Insufficient basis") if the recorded examples don't yet pin down the coefficients needed. The checked-in `cuAsm-CPP/CuAsm/InsAsmRepos/DefaultInsAsmRepos.sm_{60,61,70,75,80,86}.txt` files are a static, hand-curated snapshot of such examples (byte-identical to the original Python project's corpus, whose own docs admit sm_61/sm_86 coverage is weak) — never auto-regenerated, so any modifier combination nvcc happens to emit that the corpus never captured throws.

The fix was to feed each failing instruction's real, correctly-encoded example (extracted via `cuobjdump -sass` on each fixture's already-compiled `.orig.cubin`) into `CuInsAssemblerRepos::update()`, which solves for the new modifier/value coefficients and re-saves the repos file — exactly the workflow the class's own docs describe for extending it. Used a throwaway driver (built once, deleted after use — not part of the tree) wiring together `CuInsFeeder` + `CuInsAssemblerRepos::getDefaultRepos/update/save2file`, all of which already existed in the C++ port with no code changes needed:

- **`ExtendedAtomicOps` sm_75** (`RED.E.XOR.STRONG.GPU`): feeding the whole kernel's disassembly added not just `0_XOR` but also `0_INC`/`0_DEC` (used later in the same kernel by `RED.E.INC.STRONG.GPU`/`RED.E.DEC.STRONG.GPU`), which would otherwise have failed next. Test now passes.
- **`DevicePrintf` sm_61** (`JMX R0`): the repos had exactly one `JMX` sample (`JMX R2`), leaving the linear system underdetermined; one more real example resolved it. Test now passes.
- **`CooperativeGroups` sm_75** (`LD.E.STRONG.GPU`): `0_GPU` was a known modifier for other opcodes (e.g. `RED_ARI_R`) but never recorded for this particular `InsKey` (`LD_R_AURI`), since modifier sets are tracked per opcode shape, not shared. Adding the real example resolved `NewModi`, but the test still fails — it now reaches the same pre-existing weak-symbol bug documented as Reason 3, previously masked by the modifier error firing first. Folded into Reason 3 below; out of scope for this fix.

Verified via full 156-test suite: no regressions, `ExtendedAtomicOps` sm_75 and `DevicePrintf` sm_61 now pass byte-for-byte.

## Individual failures (current: 4 remaining)

1. **test_CheckDisasm_CooperativeGroups_sm_70** — unchanged. Binary diff at byte offset `0x7a0`: expected `0x0`, got `0x1`.

2. **test_CheckDisasm_CooperativeGroups_sm_75** — *(newly exposed)* Now gets past the modifier-table error fixed under Reason 2, but fails the same way as sm_70/80/86: binary diff at byte offset `0x798`, expected `0x0`, got `0x1`.

3. **test_CheckDisasm_CooperativeGroups_sm_80** — unchanged. Binary diff at byte offset `0x790`: expected `0x0`, got `0x1`.

4. **test_CheckDisasm_CooperativeGroups_sm_86** — unchanged. Binary diff at byte offset `0x790`: expected `0x0`, got `0x1`.

## Reasons

### Reason 1: Unresolved external symbol `vprintf` in fixup evaluation — FIXED

Was hit identically by all six `DevicePrintf` SM variants; see "Fix applied" above. Fixing it did not make sm_60/sm_61 pass automatically — it just let them progress far enough to hit further, unrelated bugs (Reason 4 for sm_60, Reason 2 for sm_61).

### Reason 2: Instruction modifier/basis table missing an entry for a given SM — FIXED

Affected `ExtendedAtomicOps` sm_75, `CooperativeGroups` sm_75, and `DevicePrintf` sm_61; see "Fix applied" above. All three now get past the modifier/basis check. `CooperativeGroups` sm_75 still fails, but for the unrelated Reason 3 below (now folded in as #2 above), previously hidden behind the modifier error.

### Reason 3: Single-bit cubin mismatch on round-trip of kernels containing weak symbols

**Affects:** #1, #2, #3, #4 (`CooperativeGroups` sm_70, sm_75, sm_80, sm_86 — all four variants, now that Reason 2 no longer masks sm_75)

All four get all the way through parsing and re-assembly, and fail only at the final binary-comparison step, with the new cubin differing from the original by exactly one bit (`0x0` vs `0x1`) at a late file offset (`0x7a0` for sm_70, `0x798` for sm_75, `0x790` for sm_80/sm_86). All four trigger a `preScan` warning: `Weak symbol found! The implementation is not complete, please be cautious...`. This warning is a direct admission in the code that weak-symbol handling is unfinished. The single flipped bit is consistent with one specific flag tied to weak-symbol/cooperative-kernel handling not being reproduced identically during re-assembly. For sm_80/sm_86 the pipeline also runs an extra "Demoting default-UR desc bits" pass during `saveAsCubin` (part of the SM_80+ descriptor-hack workaround), but since sm_70/sm_75 show the identical single-bit-diff symptom without that pass, the demoting step is not the root cause — the underlying incomplete weak-symbol handling is the common thread across all four.

### Reason 4: Two stacked bugs in `DevicePrintf` sm_60, exposed by the Reason 1 fix — FIXED

Affected `DevicePrintf` sm_60 only; see "Fix applied" above for both root causes (JMX/BRX label offset, and `.align`-clobbers-`sh_addralign`). Both are now fixed and the test passes byte-for-byte.

# Test Report — 2026-08-13 (12:27 BST)

Source: `test-results.log` (build via `build.bat -s`, full CTest run via `test.bat -s`)

**Result: 152/156 passed, 4 failed — unchanged from the post-Reason-2 state above; no regressions, no newly-exposed failures.**

No code changes accompanied this run. All 152 non-`CooperativeGroups` tests pass, including all six `DevicePrintf` SM variants (Reasons 1 and 4) and `ExtendedAtomicOps_sm_75`/`DevicePrintf_sm_61` (Reason 2). The 4 remaining failures are exactly `CooperativeGroups` sm_70/sm_75/sm_80/sm_86, still attributed to Reason 3 (incomplete weak-symbol handling; see "Reason 3" above), at the same byte offsets as before:

## Individual failures (current: 4 remaining)

1. **test_CheckDisasm_CooperativeGroups_sm_70** — unchanged. Binary diff at byte offset `0x7a0`: expected `0x0`, got `0x1`.

2. **test_CheckDisasm_CooperativeGroups_sm_75** — unchanged. Binary diff at byte offset `0x798`: expected `0x0`, got `0x1`.

3. **test_CheckDisasm_CooperativeGroups_sm_80** — unchanged. Binary diff at byte offset `0x790`: expected `0x0`, got `0x1`.

4. **test_CheckDisasm_CooperativeGroups_sm_86** — unchanged. Binary diff at byte offset `0x790`: expected `0x0`, got `0x1`.

Reason 3 remains open and is the only outstanding blocker for a fully green suite.

# Test Report — 2026-08-13 (13:52 BST)

Source: `test-results.log` (full clean rebuild via `build.bat -s`, full CTest run)

**Result: 156/156 passed, 0 failed. All four reasons are now FIXED.**

## Fix applied: Reason 3 (relocation addend bug, previously misattributed to weak-symbol handling)

The "weak symbol" label on this bug was a misdiagnosis carried over from the initial triage: the `.weak` directive actually present in each failing `CooperativeGroups` fixture (e.g. `$__internal_0_$__cuda_sm70_barrier_sync_0`) is unrelated to the byte that actually differs. Byte-diffing `CooperativeGroups.sm_70.orig.cubin` against the reassembled `.new.cubin` isolated the mismatch to a single byte inside `.nv.constant4` (file offset `0x7a0`, one byte past the section's `.dword` fixup at section-offset `0x8`). `readelf -r`/`-x` on both files showed the relocation entries and `.symtab` were byte-identical between orig and new — only the raw placeholder content at the relocated location differed (`0x00` in the original, `0x01` in the reassembled output).

That placeholder is written by a `.dword <symbol>` directive (`_ZN51_INTERNAL_..._cpo4swapE`, a local `OBJECT` symbol with `st_value=1`, disassembled from `.nv.constant4`) against a `SHT_REL` (implicit-addend) relocation section. For implicit-addend relocations, the file itself must hold only the addend (0 for a bare symbol reference, since the symbol's actual runtime address is supplied separately by the relocation entry at load time) — but `CuAsmParser::Impl::evalFixups` (`cuAsm-CPP/CuAsm/CuAsmParser.cpp`), after correctly emitting the relocation record, was *also* writing the symbol's own resolved value (`res.value`, sourced from `evalVar`'s local-label-offset branch) into the section bytes, double-counting it. This was invisible in every other fixture in the suite because the referenced symbols all happened to already have value 0 — masking the bug for two full sessions of fixes (Reasons 1, 2, 4) until this one local symbol with a nonzero value exposed it.

Fixed by computing only the arithmetic addend for the placeholder bytes when a relocation is emitted (0 for a bare symbol reference; the literal constant from any `symbol +/- N` expression otherwise), instead of `res.value`. No other fixture in the repo uses the `symbol +/- N` form for a `.word`/`.dword` relocation, so this path was previously untested but is a strict correctness fix with no observed regression risk.

Verified via full 156-test suite (fresh clean rebuild): **156/156 passing**, zero regressions. `CooperativeGroups` sm_70/sm_75/sm_80/sm_86 now pass byte-for-byte, closing out the only remaining failure category.

## Individual failures (current: none)

None. All 156 tests pass.

## Reasons

### Reason 3: Relocation addend baked into section bytes for REL-relocated symbols with nonzero value — FIXED

Previously mislabeled "weak symbol" handling; see "Fix applied" above for the actual root cause and fix. Affected `CooperativeGroups` sm_70, sm_75, sm_80, sm_86 (all four variants). All four now pass byte-for-byte.

All four reasons (1, 2, 3, 4) are now fixed. The suite is fully green: 156/156 passing.

