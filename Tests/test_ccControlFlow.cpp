#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../CuAsm/CuControlCode.hpp"
#include "../CuAsm/LatencyClass.hpp"
#include "../bin/ccCommon.hpp"
#include "utils/TestUtilsCommon.hpp"

using CuAsm::CuControlCode;
using CuAsm::LatencyClassEntry;
using CuAsm::LatencyKind;
using CuAsm::Tools::computeControlFlowSuccessors;
using CuAsm::Tools::DecodedInstruction;

namespace {

/// PT sentinel value (CuInsParser::parsePred()'s "unconditional" guard) -- see ccCommon.hpp's
/// c_PredTrueIndex.
constexpr std::int64_t c_Pt = 7;

const LatencyClassEntry c_Fixed{LatencyKind::FIXED, std::nullopt};

/**
 * @brief Builds a synthetic DecodedInstruction for computeControlFlowSuccessors()-only tests --
 *        only address/insKey/insVals/insModi matter here; roles/latency/ctrlCode are never
 *        consulted by that function, so they're filled with arbitrary valid placeholders.
 * @param address This instruction's address within its (synthetic) kernel.
 * @param insKey Opcode + operand-shape key (only the "_II"-suffix/opcode-prefix matter here).
 * @param insVals insVals[0] is the guard-predicate value; insVals.back() is the branch's
 *        immediate operand (relative offset, or -- with an "0_ABS" insModi entry -- a raw
 *        absolute address, mirroring CuInsParser::specialTreatment()'s own encoding rule).
 * @param insModi Modifier list, e.g. {"0_BRA", "0_ABS"} for a ".ABS"-suffixed opcode.
 * @return The synthetic instruction.
 **/
DecodedInstruction makeIns(std::uint64_t address, std::string insKey, std::vector<std::int64_t> insVals,
                            std::vector<std::string> insModi = {}) {
    return DecodedInstruction{
        .address = address,
        .insKey = std::move(insKey),
        .insVals = std::move(insVals),
        .insModi = std::move(insModi),
        .ctrlCode = CuControlCode(CuControlCode::mergeCode(0, 7, 7, 1, 0)),
        .roles = {},
        .latency = c_Fixed,
    };
}

} // namespace

/**
 * @brief Regression coverage for ccCommon.hpp's computeControlFlowSuccessors(): specifically, its
 *        handling (or previous lack thereof) of an ".ABS"-modified BRA/JMP immediate target.
 *        CuInsParser::specialTreatment() (CuInsParser.cpp) only converts a BRA/JMP's parsed
 *        immediate into a PC-relative offset when the opcode does *not* contain "ABS" -- for
 *        "BRA.ABS 0x...;"/"JMP.ABS 0x...;", insVals.back() is left as the raw absolute target
 *        address. A previous version of computeControlFlowSuccessors() applied the relative-offset
 *        formula (target = insVals.back() + address + instructionStride) unconditionally, which
 *        for an ABS-modified branch double-counts address+stride against an already-absolute
 *        value -- either throwing a spurious "target outside this kernel's instruction list" error
 *        for an otherwise-valid kernel, or (worse, if the miscomputed value happens to coincide
 *        with a real address) silently building an incorrect hazard graph. Fixed by checking for
 *        an "0_ABS" entry in insModi (CuInsParser::parse()'s own "0_" + opcode-dot-token
 *        convention for opcode-level modifiers) and using insVals.back() directly as the target
 *        in that case, mirroring specialTreatment()'s own check.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    // ---- Sanity check: an ordinary (non-ABS) unconditional relative BRA_II still resolves via
    // the relative-offset formula, unaffected by the ABS fix. ----
    {
        // "BRA 0x20;" at address 0x00 (16-byte stride), encoded per specialTreatment()'s own rule
        // as insVals.back() == target - address - stride == 0x20 - 0x00 - 0x10 == 0x10.
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "BRA_II", {c_Pt, 0x10}),
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
            makeIns(0x20, "MOV_R_II", {c_Pt, 0}),
        };
        std::vector<std::vector<std::size_t>> succ;
        t.checkNoThrow("an unconditional relative BRA_II decodes without throwing",
                        [&]() { succ = computeControlFlowSuccessors(instrs); });
        t.check("...and resolves to its real (offset-decoded) target only, no fallthrough",
                succ.size() == 3 && succ[0].size() == 1 && succ[0][0] == 2);
    }

    // ---- The fix: an unconditional BRA.ABS's insVals.back() is a raw absolute address and must
    // be used directly, not added to address+stride. ----
    {
        // "BRA.ABS 0x20;" at address 0x00 -- insVals.back() is literally 0x20, the absolute
        // target. Before the fix this resolved to 0x20 + 0x00 + 0x10 == 0x30, outside this
        // 3-instruction kernel's address range, and would have thrown.
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "BRA_II", {c_Pt, 0x20}, {"0_BRA", "0_ABS"}),
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
            makeIns(0x20, "MOV_R_II", {c_Pt, 0}),
        };
        std::vector<std::vector<std::size_t>> succ;
        t.checkNoThrow("an unconditional BRA.ABS decodes without throwing",
                        [&]() { succ = computeControlFlowSuccessors(instrs); });
        t.check("...and resolves to its real absolute target (index 2), not address+stride+target",
                succ.size() == 3 && succ[0].size() == 1 && succ[0][0] == 2);
    }

    // ---- A conditionally-guarded BRA.ABS still yields both the resolved absolute target and the
    // fallthrough successor (guard handling is orthogonal to the ABS/relative distinction). ----
    {
        // "@P0 BRA.ABS 0x20;" at address 0x00 -- insVals[0] == 0 (a real predicate, not PT/7).
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "BRA_II", {0, 0x20}, {"0_BRA", "0_ABS"}),
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
            makeIns(0x20, "MOV_R_II", {c_Pt, 0}),
        };
        std::vector<std::vector<std::size_t>> succ;
        t.checkNoThrow("a conditionally-guarded BRA.ABS decodes without throwing",
                        [&]() { succ = computeControlFlowSuccessors(instrs); });
        const bool hasTarget = succ.size() == 3 && std::find(succ[0].begin(), succ[0].end(), std::size_t{2}) != succ[0].end();
        const bool hasFallthrough = succ.size() == 3 && std::find(succ[0].begin(), succ[0].end(), std::size_t{1}) != succ[0].end();
        t.check("...and resolves both its absolute target and its fallthrough successor",
                succ.size() == 3 && succ[0].size() == 2 && hasTarget && hasFallthrough);
    }

    // ---- JMP.ABS: the same ".ABS" handling applies to JMP, not just BRA ----
    {
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "JMP_II", {c_Pt, 0x20}, {"0_JMP", "0_ABS"}),
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
            makeIns(0x20, "MOV_R_II", {c_Pt, 0}),
        };
        std::vector<std::vector<std::size_t>> succ;
        t.checkNoThrow("an unconditional JMP.ABS decodes without throwing",
                        [&]() { succ = computeControlFlowSuccessors(instrs); });
        t.check("...and resolves to its real absolute target, same as BRA.ABS", succ.size() == 3 && succ[0].size() == 1 &&
                                                                                     succ[0][0] == 2);
    }

    // ---- CALL/CAL: always fallthrough-only, regardless of guard -- the callee lives in a
    // different section entirely, so its target is irrelevant to this kernel's own CFG. ----
    {
        const std::vector<DecodedInstruction> conditional = {
            makeIns(0x00, "CALL_R", {0, 4}),
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
        };
        const auto succCond = computeControlFlowSuccessors(conditional);
        t.check("a conditionally-guarded CALL is fallthrough-only", succCond[0].size() == 1 && succCond[0][0] == 1);

        const std::vector<DecodedInstruction> deadGuard = {
            makeIns(0x00, "CALL_R", {15, 4}), // "@!PT CALL R4;" -- guard is AlwaysFalse
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
        };
        const auto succDead = computeControlFlowSuccessors(deadGuard);
        t.check("a dead-guarded (@!PT) CALL is still fallthrough-only, not skipped entirely",
                succDead[0].size() == 1 && succDead[0][0] == 1);
    }

    // ---- BSSY/SSY/PBK/PRET: convergence-stack bookkeeping, always fallthrough-only ----
    {
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "BSSY_B_II", {c_Pt, 0x1000}), // target operand present but never interpreted
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
        };
        const auto succ = computeControlFlowSuccessors(instrs);
        t.check("BSSY is fallthrough-only; its own operand is never resolved as a jump target",
                succ[0].size() == 1 && succ[0][0] == 1);
    }

    // ---- EXIT/RET: no successor at all when unconditional, fallthrough-only when guarded ----
    {
        const std::vector<DecodedInstruction> unconditional = {
            makeIns(0x00, "EXIT", {c_Pt}),
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
        };
        const auto succUncond = computeControlFlowSuccessors(unconditional);
        t.check("an unconditional EXIT has no successor at all, even though a fallthrough slot exists",
                succUncond[0].empty());
    }
    {
        const std::vector<DecodedInstruction> conditional = {
            makeIns(0x00, "EXIT", {0}), // "@P0 EXIT;"
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
        };
        const auto succCond = computeControlFlowSuccessors(conditional);
        t.check("a conditionally-guarded EXIT falls through for the (real) not-taken path",
                succCond[0].size() == 1 && succCond[0][0] == 1);
    }
    {
        const std::vector<DecodedInstruction> deadGuard = {
            makeIns(0x00, "EXIT", {15}), // "@!PT EXIT;" -- dead code, never actually exits
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
        };
        const auto succDead = computeControlFlowSuccessors(deadGuard);
        t.check("a dead-guarded (@!PT) EXIT is treated as straight-line fallthrough", succDead[0].size() == 1 && succDead[0][0] == 1);
    }
    {
        // A conditionally-guarded RET as the kernel's very last instruction: no fallthrough slot
        // exists at all (hasFallthrough == false), regardless of the guard not being AlwaysTrue.
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "MOV_R_II", {c_Pt, 0}),
            makeIns(0x10, "RET", {0}), // "@P0 RET;", last instruction
        };
        const auto succ = computeControlFlowSuccessors(instrs);
        t.check("a conditionally-guarded RET with no fallthrough slot available (last instruction) has no successor",
                succ[1].empty());
    }

    // ---- Indirect/computed branches are a hard error, never a guess ----
    {
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "BRX_R", {c_Pt, 4}), // register-indirect target -- opcode isn't BRA/JMP at all
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
        };
        t.checkThrows<std::runtime_error>("an indirect BRX throws rather than guessing a target",
                                            [&]() { (void)computeControlFlowSuccessors(instrs); });
    }
    {
        // A "BRA" whose InsKey doesn't end in "_II" (e.g. a hypothetical register-operand form) is
        // just as unresolvable as BRX -- the opcode alone isn't sufficient, the operand shape matters.
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "BRA_R", {c_Pt, 4}),
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
        };
        t.checkThrows<std::runtime_error>("a non-\"_II\" BRA (not a plain immediate target) throws, same as an indirect branch",
                                            [&]() { (void)computeControlFlowSuccessors(instrs); });
    }
    {
        // A resolvable-shaped BRA_II whose computed target doesn't land on any real instruction
        // address in this kernel is also a hard error, not silently ignored.
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "BRA_II", {c_Pt, 0x1000}), // way outside this 2-instruction kernel's range
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
        };
        t.checkThrows<std::runtime_error>("a BRA_II whose resolved target lands outside the kernel's instruction list throws",
                                            [&]() { (void)computeControlFlowSuccessors(instrs); });
    }
    {
        // The same out-of-range immediate, but on a dead (@!PT) branch: since an AlwaysFalse guard
        // is never actually taken, its target is never resolved at all -- so this must NOT throw,
        // proving dead branches are skipped before target resolution, not merely tolerated by luck.
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "BRA_II", {15, 0x7fffffff}), // "@!PT BRA 0x7fffffff;"
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
        };
        std::vector<std::vector<std::size_t>> succ;
        t.checkNoThrow("a dead (@!PT) branch with a garbage/unresolvable target never attempts to resolve it",
                        [&]() { succ = computeControlFlowSuccessors(instrs); });
        t.check("...and resolves to plain fallthrough only", succ.size() == 2 && succ[0].size() == 1 && succ[0][0] == 1);
    }

    // ---- computeControlFlowSuccessors() itself is direction-agnostic: a backward (loop) branch
    // resolves its target exactly like a forward one -- filtering backward edges out of the hazard
    // dataflow is buildHazardGraph()'s job, not this function's. ----
    {
        // "BRA 0x00;" at address 0x20 -- an unconditional loop-back branch to the kernel's first
        // instruction. Encoded per specialTreatment()'s own rule: insVals.back() ==
        // target - address - stride == 0x00 - 0x20 - 0x10 == -0x30.
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "MOV_R_II", {c_Pt, 0}),
            makeIns(0x10, "MOV_R_II", {c_Pt, 0}),
            makeIns(0x20, "BRA_II", {c_Pt, -0x30}),
        };
        const auto succ = computeControlFlowSuccessors(instrs);
        t.check("an unconditional backward branch resolves to its real (earlier) target index",
                succ[2].size() == 1 && succ[2][0] == 0);
    }

    // ---- A plain instruction at the very end of a kernel (no fallthrough slot available) has no
    // successors at all -- the ordinary (non-branch, non-EXIT/RET) opcode path. ----
    {
        const std::vector<DecodedInstruction> instrs = {
            makeIns(0x00, "MOV_R_II", {c_Pt, 0}),
            makeIns(0x10, "IADD3_R_R_R_R", {c_Pt, 4, 4, 4, 255}),
        };
        const auto succ = computeControlFlowSuccessors(instrs);
        t.check("an ordinary instruction with no fallthrough slot (last in the kernel) has no successors", succ[1].empty());
    }

    return t.finish("test_ccControlFlow");
}
