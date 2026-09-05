#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../CuAsm/CuControlCode.hpp"
#include "../CuAsm/CuSMVersion.hpp"
#include "../CuAsm/LatencyClass.hpp"
#include "../CuAsm/OperandRole.hpp"
#include "../bin/ccCommon.hpp"
#include "utils/TestUtilsCommon.hpp"

using CuAsm::AccessMode;
using CuAsm::BarrierType;
using CuAsm::CuControlCode;
using CuAsm::LatencyClassEntry;
using CuAsm::LatencyKind;
using CuAsm::OperandKind;
using CuAsm::OperandRoleEntry;
using CuAsm::Tools::assignBarrierSlots;
using CuAsm::Tools::BarrierAssignment;
using CuAsm::Tools::BarrierInterval;
using CuAsm::Tools::buildHazardGraph;
using CuAsm::Tools::c_ScoreboardSlotCount;
using CuAsm::Tools::CheckStatus;
using CuAsm::Tools::collectBarrierIntervals;
using CuAsm::Tools::ControlCodeCheckResult;
using CuAsm::Tools::correctControlCodes;
using CuAsm::Tools::DecodedInstruction;
using CuAsm::Tools::isBarrierEligible;
using CuAsm::Tools::KernelControlCodes;
using CuAsm::Tools::relevantAccessOf;
using CuAsm::Tools::simulateAndVerify;
using CuAsm::Tools::verifyControlCodes;

namespace {

/// PT/UPT sentinel (see ccCommon.hpp's c_PredTrueIndex) -- unconditional guard.
constexpr std::int64_t c_Pt = 7;

const LatencyClassEntry c_Fixed{LatencyKind::FIXED, std::nullopt};
const LatencyClassEntry c_VariableWrite{LatencyKind::VARIABLE, BarrierType::WRITE};
const LatencyClassEntry c_VariableRead{LatencyKind::VARIABLE, BarrierType::READ};

/**
 * @brief Builds a synthetic DecodedInstruction by hand, exactly like test_ccHazard.cpp's own
 *        makeIns() -- this file exercises Reports/tasks.md Phase 4's correction layer, built
 *        directly on top of Phase 3's hazard graph/simulator, so it uses the same "hand-built
 *        instruction sequence, no cuobjdump/real cubin" approach that file established.
 * @param insKey Label for this instruction (only used for readability/debugging here).
 * @param insVals insVals[0] is the guard-predicate value (7 == PT/unconditional); insVals[1..]
 *        align 1:1 with roles.
 * @param roles Per-operand roles, aligned with insVals[1..].
 * @param latency This instruction's latency classification.
 * @param ctrl Raw control code (see CuControlCode::mergeCode()).
 * @return The synthetic instruction.
 **/
DecodedInstruction makeIns(std::string insKey, std::vector<std::int64_t> insVals, std::vector<OperandRoleEntry> roles,
                            LatencyClassEntry latency, std::uint32_t ctrl) {
    return DecodedInstruction{
        .address = 0,
        .insKey = std::move(insKey),
        .insVals = std::move(insVals),
        .insModi = {},
        .ctrlCode = CuControlCode(ctrl),
        .roles = std::move(roles),
        .latency = latency,
    };
}

/// Convenience for building a KernelControlCodes whose ctrlCodeList mirrors decoded's own
/// (already-split) control codes -- correctControlCodes() never reads kcc.ctrlCodeList itself
/// (only decoded's), but tests that assert "left unchanged on Unrepairable" need a baseline to
/// diff against.
KernelControlCodes makeKcc(const std::vector<DecodedInstruction>& decoded) {
    KernelControlCodes kcc;
    kcc.kernelName = "testKernel";
    for (const DecodedInstruction& ins : decoded) {
        std::uint32_t waitbar = 0;
        for (int slot : ins.ctrlCode.getBarrierSet()) {
            waitbar |= (1u << slot);
        }
        const std::uint32_t readbar = ins.ctrlCode.getReadSB() < 0 ? 7u : static_cast<std::uint32_t>(ins.ctrlCode.getReadSB());
        const std::uint32_t writebar = ins.ctrlCode.getWriteSB() < 0 ? 7u : static_cast<std::uint32_t>(ins.ctrlCode.getWriteSB());
        const std::uint32_t yieldFlag = ins.ctrlCode.isYield() ? 0u : 1u;
        kcc.ctrlCodeList.push_back(CuControlCode::mergeCode(waitbar, readbar, writebar, yieldFlag, ins.ctrlCode.getStallCount()));
    }
    return kcc;
}

} // namespace

/**
 * @brief Exercises Reports/tasks.md Phase 4: the barrier-id interval-coloring allocator
 *        (collectBarrierIntervals()/assignBarrierSlots()) and the correctControlCodes() entry
 *        point built on top of it and Phase 3's simulateAndVerify(), against hand-built synthetic
 *        instruction sequences -- no cuobjdump/real cubin needed (test_ccCommon.cpp's job).
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    // ---- relevantAccessOf() / isBarrierEligible() ----
    {
        t.check("relevantAccessOf(RAW) is WRITE", relevantAccessOf(CuAsm::Tools::HazardType::RAW) == BarrierType::WRITE);
        t.check("relevantAccessOf(WAW) is WRITE", relevantAccessOf(CuAsm::Tools::HazardType::WAW) == BarrierType::WRITE);
        t.check("relevantAccessOf(WAR) is READ", relevantAccessOf(CuAsm::Tools::HazardType::WAR) == BarrierType::READ);

        t.check("a VARIABLE:WRITE producer is barrier-eligible for a WRITE-relevant edge",
                isBarrierEligible(c_VariableWrite, BarrierType::WRITE));
        t.check("a VARIABLE:WRITE producer is NOT barrier-eligible for a READ-relevant edge (its other access)",
                !isBarrierEligible(c_VariableWrite, BarrierType::READ));
        t.check("a FIXED producer is never barrier-eligible", !isBarrierEligible(c_Fixed, BarrierType::WRITE) &&
                                                                    !isBarrierEligible(c_Fixed, BarrierType::READ));

        t.check("a VARIABLE:READ producer is barrier-eligible for a READ-relevant (WAR) edge",
                isBarrierEligible(c_VariableRead, BarrierType::READ));
        t.check("a VARIABLE:READ producer is NOT barrier-eligible for a WRITE-relevant edge (its other access)",
                !isBarrierEligible(c_VariableRead, BarrierType::WRITE));
    }

    // ---- collectBarrierIntervals() ----
    {
        // 0: VARIABLE:WRITE producer on R4, genuinely adjacent to its nearest consumer with zero
        //    natural stall margin -- the one case that can't be discharged by natural spacing
        //    alone (Reports/correct-cc-overconservative-barrier-intervals.md).
        // 1: reads R4 (RAW, barrier-eligible) -- first consumer.
        // 2: reads R4 again -- must NOT create a second interval, only widen/ignore since 1 < 2.
        const std::vector<DecodedInstruction> instrs = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, 0, 1, /*stall=*/0)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_R", {c_Pt, 6, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto graph = buildHazardGraph(instrs);
        const auto intervals = collectBarrierIntervals(instrs, graph);
        t.check("collectBarrierIntervals() finds exactly one interval for a producer with two eligible consumers",
                intervals.size() == 1);
        t.check("the interval's firstConsumerIndex is the *earliest* consumer (1), not the latest (2)",
                !intervals.empty() && intervals[0].producerIndex == 0 && intervals[0].firstConsumerIndex == 1 &&
                    intervals[0].direction == BarrierType::WRITE);
    }
    {
        // A VARIABLE:WRITE producer whose only outgoing edge is a WAR on its *other* (address)
        // access must NOT get a barrier interval -- see isBarrierEligible()'s doc.
        const std::vector<DecodedInstruction> instrs = {
            makeIns("LDG_R_ARI", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::R_ADDR, AccessMode::READ}},
                    c_VariableWrite, CuControlCode::mergeCode(0, 7, 0, 1, 1)),
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto graph = buildHazardGraph(instrs);
        t.check("a VARIABLE producer's non-barrier-protected (WAR-only) access yields no barrier interval",
                collectBarrierIntervals(instrs, graph).empty());
    }
    {
        // Regression for Reports/correct-cc-overconservative-barrier-intervals.md: a VARIABLE
        // producer whose *nearest* barrier-eligible consumer is not immediately adjacent already
        // has enough natural instruction spacing to be safe under this codebase's own
        // FIXED-latency approximation (simulateAndVerify()'s doc) -- it must get NO interval at
        // all. "UNCURATED_R_ARI" isn't a real opcode -- deliberately not one of
        // minSafeBarrierMargin()'s curated keys, so this exercises the uncurated default margin
        // of 1 in isolation, robust against that table growing new real entries later.
        const std::vector<DecodedInstruction> instrs = {
            makeIns("UNCURATED_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, 7, 1, /*stall=*/1)),
            makeIns("MOV_R_II", {c_Pt, 5}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_R", {c_Pt, 6, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto graph = buildHazardGraph(instrs);
        t.check("a VARIABLE producer with a non-adjacent nearest consumer needs no barrier interval at all",
                collectBarrierIntervals(instrs, graph).empty());
    }
    {
        // A VARIABLE producer immediately adjacent to its nearest consumer, but already carrying
        // a nonzero natural stall count, is *also* safe under the uncurated default margin of 1 --
        // adjacency alone isn't the trigger, the margin (distance + stall) clearing the threshold
        // is. Same deliberately-fake "UNCURATED_R_ARI" opcode as above.
        const std::vector<DecodedInstruction> instrs = {
            makeIns("UNCURATED_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, 7, 1, /*stall=*/1)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto graph = buildHazardGraph(instrs);
        t.check("an adjacent VARIABLE producer with a nonzero natural stall count needs no barrier interval either",
                collectBarrierIntervals(instrs, graph).empty());
    }
    {
        // minSafeBarrierMargin() regression: DMUL_R_R_R is curated with a minimum safe margin of
        // 48 (arXiv:1903.07486's DP-pipe dependent-issue latency figure), well above the uncurated
        // default of 1. Input has no pre-existing barrier (writebar unset), so this exercises the
        // margin logic itself, not collectBarrierIntervals()'s safety net. A margin that would
        // satisfy the *default* threshold (distance 3 + stall 1 = 4) must still be judged unsafe
        // for this specific curated opcode, and still get a real interval. NOTE: LDS_R_ARI is
        // deliberately NOT used here any more -- see minSafeBarrierMargin()'s own doc for why a
        // curated LDS_R_ARI margin was proven unsound and removed
        // (Reports/correct-cc-margin-heuristic-strips-real-barriers.md).
        const std::vector<DecodedInstruction> instrs = {
            makeIns("DMUL_R_R_R", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, 7, 1, /*stall=*/1)),
            makeIns("MOV_R_II", {c_Pt, 5}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_II", {c_Pt, 6}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_II", {c_Pt, 7}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_R", {c_Pt, 8, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto graph = buildHazardGraph(instrs);
        const auto intervals = collectBarrierIntervals(instrs, graph);
        t.check("a curated opcode below its own minimum safe margin still needs a real interval, "
                "even past the uncurated default threshold",
                intervals.size() == 1 && intervals[0].producerIndex == 0 && intervals[0].firstConsumerIndex == 4);
    }
    {
        // Same shape, but with exactly margin 48 (distance 47 + stall 1) -- DMUL_R_R_R's curated
        // threshold. This must stay interval-free (margin clears the bar, and the input has no
        // pre-existing barrier for the safety net to preserve either).
        std::vector<DecodedInstruction> instrs;
        instrs.push_back(makeIns("DMUL_R_R_R", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                                  CuControlCode::mergeCode(0, 7, 7, 1, /*stall=*/1)));
        for (int i = 0; i < 47; ++i) {
            instrs.push_back(makeIns("MOV_R_II", {c_Pt, 5}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                                      CuControlCode::mergeCode(0, 7, 7, 1, 1)));
        }
        instrs.push_back(makeIns("MOV_R_R", {c_Pt, 8, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}},
                                  c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)));
        const auto graph = buildHazardGraph(instrs);
        t.check("a curated opcode exactly at its own minimum safe margin needs no interval",
                collectBarrierIntervals(instrs, graph).empty());
    }
    {
        // Safety net regression for Reports/correct-cc-margin-heuristic-strips-real-barriers.md:
        // a producer whose margin clears its curated threshold, but whose *input* control code
        // already has a real barrier set for this access, must still get a real interval -- the
        // margin heuristic must never erase direct, first-party evidence a producer needed
        // protection. Same shape as the "exactly at margin" case above, except writebar is
        // pre-set (slot 0) on the producer.
        std::vector<DecodedInstruction> instrs;
        instrs.push_back(makeIns("DMUL_R_R_R", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                                  CuControlCode::mergeCode(0, 7, /*writebar=*/0, 1, /*stall=*/1)));
        for (int i = 0; i < 47; ++i) {
            instrs.push_back(makeIns("MOV_R_II", {c_Pt, 5}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                                      CuControlCode::mergeCode(0, 7, 7, 1, 1)));
        }
        instrs.push_back(makeIns("MOV_R_R", {c_Pt, 8, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}},
                                  c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)));
        const auto graph = buildHazardGraph(instrs);
        const auto intervals = collectBarrierIntervals(instrs, graph);
        t.check("a producer whose input already has a real barrier keeps its interval even past "
                "its curated safe margin",
                intervals.size() == 1 && intervals[0].producerIndex == 0);
    }

    // ---- assignBarrierSlots() ----
    {
        // Two non-overlapping intervals (touching at the boundary) may share a slot.
        const std::vector<BarrierInterval> intervals = {
            {0, 2, BarrierType::WRITE, "OPCODE_A"},
            {2, 4, BarrierType::WRITE, "OPCODE_B"},
        };
        const auto assignment = assignBarrierSlots(intervals);
        t.check("assignBarrierSlots() succeeds for two touching (non-overlapping) intervals", assignment.has_value());
        t.check("two touching intervals are packed into a single slot",
                assignment.has_value() && assignment->slotsUsed == 1);
    }
    {
        // Two genuinely overlapping intervals of *different* insKeys must get distinct slots --
        // this codebase has no confirmed FIFO-completion-order evidence across different
        // instruction classes (assignBarrierSlots()'s own doc).
        const std::vector<BarrierInterval> intervals = {
            {0, 3, BarrierType::WRITE, "OPCODE_A"},
            {1, 4, BarrierType::WRITE, "OPCODE_B"},
        };
        const auto assignment = assignBarrierSlots(intervals);
        t.check("assignBarrierSlots() succeeds for two overlapping intervals", assignment.has_value());
        t.check("two overlapping intervals of different insKeys need two distinct slots",
                assignment.has_value() && assignment->slotsUsed == 2 &&
                    assignment->slotOf.at(0) != assignment->slotOf.at(1));
    }
    {
        // Regression for Reports/correct-cc-shared-slot-reuse-not-modeled.md: two genuinely
        // overlapping intervals of the *same* insKey may share one physical slot -- real,
        // ptxas-emitted control codes do exactly this (Reports/gemm-tiled-repro.sass: several
        // back-to-back same-opcode producers tagged to one slot, retired by a single later wait).
        const std::vector<BarrierInterval> intervals = {
            {0, 3, BarrierType::WRITE, "LDS_R_ARI"},
            {1, 4, BarrierType::WRITE, "LDS_R_ARI"},
        };
        const auto assignment = assignBarrierSlots(intervals);
        t.check("assignBarrierSlots() succeeds for two overlapping same-insKey intervals", assignment.has_value());
        t.check("...and packs them into a single shared slot",
                assignment.has_value() && assignment->slotsUsed == 1);
    }
    {
        // Regression for Reports/correct-cc-insKey-sharing-too-broad.md: 7 mutually-overlapping,
        // contiguous, same-insKey intervals must NOT all collapse into one shared slot -- only the
        // confirmed-safe c_MaxShareableGroupSize (3) may ever share, so this splits into three
        // share groups (sizes 3, 3, 1) and, since every interval overlaps every other, needs one
        // distinct slot per group.
        std::vector<BarrierInterval> intervals;
        for (std::size_t i = 0; i < 7; ++i) {
            intervals.push_back(BarrierInterval{i, static_cast<std::size_t>(8), BarrierType::WRITE, "LDS_R_ARI"});
        }
        const auto assignment = assignBarrierSlots(intervals);
        t.check("assignBarrierSlots() succeeds for 7 overlapping same-insKey intervals", assignment.has_value());
        t.check("...but caps sharing at 3 producers per slot, needing 3 slots (not 1)",
                assignment.has_value() && assignment->slotsUsed == 3);
    }
    {
        // Regression for Reports/correct-cc-insKey-sharing-too-broad.md: two same-insKey,
        // overlapping intervals with a *different*-insKey producer's interval issued in between
        // (in producerIndex order) are not contiguous, so they must NOT be treated as shareable
        // even though the insKey itself matches and they still overlap.
        const std::vector<BarrierInterval> intervals = {
            {0, 5, BarrierType::WRITE, "LDS_R_ARI"},
            {1, 6, BarrierType::WRITE, "OPCODE_X"},
            {2, 7, BarrierType::WRITE, "LDS_R_ARI"},
        };
        const auto assignment = assignBarrierSlots(intervals);
        t.check("assignBarrierSlots() succeeds for a same-insKey pair split by an intervening different insKey",
                assignment.has_value());
        t.check("...and does not share the split same-insKey pair's slot",
                assignment.has_value() && assignment->slotsUsed == 3 && assignment->slotOf.at(0) != assignment->slotOf.at(2));
    }
    {
        // 7 intervals of 7 *distinct* insKeys, all mutually overlapping (all start before a
        // shared end point), exceed the 6 physical scoreboard slots -- the "provably unrepairable
        // in place" case. Distinct insKeys keep this exercising the "different classes can't
        // share" path, not the new same-insKey sharing relaxation.
        std::vector<BarrierInterval> intervals;
        for (std::size_t i = 0; i < static_cast<std::size_t>(c_ScoreboardSlotCount) + 1; ++i) {
            intervals.push_back(BarrierInterval{i, static_cast<std::size_t>(c_ScoreboardSlotCount) + 1, BarrierType::WRITE,
                                                 "OPCODE_" + std::to_string(i)});
        }
        t.check("assignBarrierSlots() reports no valid assignment for more simultaneous distinct-insKey "
                "intervals than physical slots",
                !assignBarrierSlots(intervals).has_value());
    }
    {
        // getReadSB()/getWriteSB() draw from the same 6-id physical slot pool (Reports/tasks.md
        // Phase 1.1's confirmation that a real instruction can set both simultaneously) -- two
        // overlapping intervals of different insKeys must need distinct slots even when their
        // *direction* differs too.
        const std::vector<BarrierInterval> intervals = {
            {0, 3, BarrierType::WRITE, "OPCODE_A"},
            {1, 4, BarrierType::READ, "OPCODE_B"},
        };
        const auto assignment = assignBarrierSlots(intervals);
        t.check("assignBarrierSlots() succeeds for two overlapping intervals of different directions", assignment.has_value());
        t.check("...and still assigns them distinct slots, since direction doesn't partition the slot pool",
                assignment.has_value() && assignment->slotsUsed == 2 && assignment->slotOf.at(0) != assignment->slotOf.at(1));
    }

    // ---- correctControlCodes(): closes an open VARIABLE:WRITE RAW hazard ----
    {
        const std::vector<DecodedInstruction> decoded = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(/*waitbar=*/0, 7, /*writebar=*/0, /*yieldFlag=*/1, /*stall=*/2)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(/*waitbar=*/0, 7, 7, /*yieldFlag=*/0, /*stall=*/1)),
        };
        t.check("the synthetic input actually starts out violated (sanity check on the test itself)",
                verifyControlCodes(decoded, CuAsm::CuSMVersion(75)).status == CheckStatus::Violated);

        KernelControlCodes kcc = makeKcc(decoded);
        const ControlCodeCheckResult result = correctControlCodes(kcc, decoded, CuAsm::CuSMVersion(75));
        t.check("correctControlCodes() reports Corrected for a repairable open RAW hazard",
                result.status == CheckStatus::Corrected && !result.message.empty());
        t.checkEqual("kcc.ctrlCodeList keeps its original length", kcc.ctrlCodeList.size(), decoded.size());

        std::vector<DecodedInstruction> reChecked = decoded;
        for (std::size_t i = 0; i < reChecked.size(); ++i) {
            reChecked[i].ctrlCode = CuControlCode(kcc.ctrlCodeList[i]);
        }
        const auto reVerified = verifyControlCodes(reChecked, CuAsm::CuSMVersion(75));
        t.check("the corrected control codes re-verify as Verified with zero violations",
                reVerified.status == CheckStatus::Verified && reVerified.violations.empty());
        t.check("correction preserves each instruction's original yield flag",
                reChecked[0].ctrlCode.isYield() == decoded[0].ctrlCode.isYield() &&
                    reChecked[1].ctrlCode.isYield() == decoded[1].ctrlCode.isYield());
    }

    // ---- correctControlCodes(): bumps a zero-stall FIXED-latency hazard's stall count ----
    {
        const std::vector<DecodedInstruction> decoded = {
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, /*stall=*/0)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        KernelControlCodes kcc = makeKcc(decoded);
        const ControlCodeCheckResult result = correctControlCodes(kcc, decoded, CuAsm::CuSMVersion(75));
        t.check("correctControlCodes() reports Corrected for a repairable FIXED-latency zero-stall hazard",
                result.status == CheckStatus::Corrected);
        const CuControlCode fixedProducer(kcc.ctrlCodeList[0]);
        t.check("the producer's stall count was raised above zero", fixedProducer.getStallCount() >= 1);
    }

    // ---- correctControlCodes(): an already-fully-correct sequence stays Corrected and clean ----
    {
        const std::vector<DecodedInstruction> decoded = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, /*writebar=*/0, /*yieldFlag=*/0, 2)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(/*waitbar=*/0b000001, 7, 7, 1, 1)),
        };
        t.check("the synthetic input for this case is already Verified (sanity check on the test itself)",
                verifyControlCodes(decoded, CuAsm::CuSMVersion(75)).status == CheckStatus::Verified);

        KernelControlCodes kcc = makeKcc(decoded);
        const ControlCodeCheckResult result = correctControlCodes(kcc, decoded, CuAsm::CuSMVersion(75));
        t.check("correcting an already-correct sequence still reports Corrected", result.status == CheckStatus::Corrected);

        std::vector<DecodedInstruction> reChecked = decoded;
        for (std::size_t i = 0; i < reChecked.size(); ++i) {
            reChecked[i].ctrlCode = CuControlCode(kcc.ctrlCodeList[i]);
        }
        t.check("re-verifying an already-correct, re-corrected sequence still reports Verified",
                verifyControlCodes(reChecked, CuAsm::CuSMVersion(75)).status == CheckStatus::Verified);
    }

    // ---- correctControlCodes(): closes an open VARIABLE:READ WAR hazard (not just RAW/WRITE) ----
    {
        // 0: "STG [R4], R5;"-shaped producer -- VARIABLE:READ, protects its still-in-flight read
        //    of R4 (the address it's writing through). Starts with no read barrier opened at all
        //    *and* a zero stall count, so the FIXED-latency fallback rule doesn't accidentally mask
        //    the open hazard (see simulateAndVerify()'s "producer actually opens the barrier" check).
        // 1: overwrites R4 immediately -- an open WAR hazard.
        const std::vector<DecodedInstruction> decoded = {
            makeIns("STG_ARI_R", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::READ}}, c_VariableRead,
                    CuControlCode::mergeCode(/*waitbar=*/0, /*readbar=*/7, /*writebar=*/7, /*yieldFlag=*/1, /*stall=*/0)),
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        t.check("the synthetic WAR input actually starts out violated (sanity check on the test itself)",
                verifyControlCodes(decoded, CuAsm::CuSMVersion(75)).status == CheckStatus::Violated);

        KernelControlCodes kcc = makeKcc(decoded);
        const ControlCodeCheckResult result = correctControlCodes(kcc, decoded, CuAsm::CuSMVersion(75));
        t.check("correctControlCodes() reports Corrected for a repairable open VARIABLE:READ WAR hazard",
                result.status == CheckStatus::Corrected);

        const CuControlCode producer(kcc.ctrlCodeList[0]);
        t.check("the producer's READ barrier (not WRITE) was opened to close the WAR hazard",
                producer.getReadSB() != -1 && producer.getWriteSB() == -1);

        std::vector<DecodedInstruction> reChecked = decoded;
        for (std::size_t i = 0; i < reChecked.size(); ++i) {
            reChecked[i].ctrlCode = CuControlCode(kcc.ctrlCodeList[i]);
        }
        t.check("the corrected WAR hazard re-verifies clean", verifyControlCodes(reChecked, CuAsm::CuSMVersion(75)).status ==
                                                                    CheckStatus::Verified);
    }

    // ---- correctControlCodes(): never lowers an existing stall count below what it already was,
    // even when the hazard model itself only ever requires 1 ----
    {
        // Producer already has stall=5 (well above what a zero-stall adjacency check would need);
        // the FIXED-latency hazard is already satisfied, but correction recomputes requiredStall
        // unconditionally for every adjacent edge -- max(5, 1) must stay 5, not drop to 1.
        const std::vector<DecodedInstruction> decoded = {
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, /*stall=*/5)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        KernelControlCodes kcc = makeKcc(decoded);
        const ControlCodeCheckResult result = correctControlCodes(kcc, decoded, CuAsm::CuSMVersion(75));
        t.check("correctControlCodes() reports Corrected for an already-satisfied FIXED-latency hazard",
                result.status == CheckStatus::Corrected);
        const CuControlCode fixedProducer(kcc.ctrlCodeList[0]);
        t.checkEqual("the producer's stall count of 5 is preserved exactly, not lowered to the required minimum of 1",
                     fixedProducer.getStallCount(), std::uint32_t{5});
    }

    // ---- correctControlCodes(): two non-overlapping VARIABLE producers share one physical slot,
    // end to end (not just at the assignBarrierSlots() unit level) ----
    {
        // 0: VARIABLE:WRITE producer A on R4.
        // 1: consumer of A (RAW) -- A's interval is [0, 1), closed here.
        // 2: VARIABLE:WRITE producer B on R6 -- starts only once A's interval has already ended.
        // 3: consumer of B (RAW) -- B's interval is [2, 3), never overlapping A's.
        const std::vector<DecodedInstruction> decoded = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, 0, 1, /*stall=*/0)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("LDG_R_ARI", {c_Pt, 6}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, 0, 1, /*stall=*/0)),
            makeIns("MOV_R_R", {c_Pt, 7, 6}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        KernelControlCodes kcc = makeKcc(decoded);
        const ControlCodeCheckResult result = correctControlCodes(kcc, decoded, CuAsm::CuSMVersion(75));
        t.check("correctControlCodes() reports Corrected for two non-overlapping VARIABLE producers", result.status == CheckStatus::Corrected);
        t.check("...using exactly one scoreboard slot (they don't overlap in time)", result.message.find("1/6") != std::string::npos);

        const CuControlCode producerA(kcc.ctrlCodeList[0]);
        const CuControlCode producerB(kcc.ctrlCodeList[2]);
        t.checkEqual("both non-overlapping producers are assigned the identical physical slot",
                     producerA.getWriteSB(), producerB.getWriteSB());

        std::vector<DecodedInstruction> reChecked = decoded;
        for (std::size_t i = 0; i < reChecked.size(); ++i) {
            reChecked[i].ctrlCode = CuControlCode(kcc.ctrlCodeList[i]);
        }
        t.check("the shared-slot correction re-verifies clean", verifyControlCodes(reChecked, CuAsm::CuSMVersion(75)).status ==
                                                                      CheckStatus::Verified);
    }

    // ---- correctControlCodes(): two *genuinely* adjacent-with-zero-stall VARIABLE producers,
    // back to back, still get distinct slots, end to end (assignBarrierSlots() is exercised
    // directly for the general overlapping-intervals case above; this checks the same allocator
    // is actually wired up correctly through collectBarrierIntervals()/correctControlCodes()) ----
    {
        // 0: VARIABLE:WRITE producer A on R4, immediately consumed next -- genuinely adjacent,
        //    zero-stall, so A needs a real interval [0, 1).
        // 1: consumer of A (RAW) *and* itself VARIABLE:WRITE producer B on R6, immediately
        //    consumed next -- B needs its own real interval [1, 2). A's interval only touches
        //    B's (freed the instant A is retired, per BarrierInterval's own doc), so this alone
        //    doesn't force distinct slots -- see the assignBarrierSlots() unit tests above for the
        //    genuinely-overlapping case at that layer.
        // 2: consumer of B (RAW).
        const std::vector<DecodedInstruction> decoded = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, 0, 1, /*stall=*/0)),
            makeIns("LDG_R_R", {c_Pt, 6, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}},
                    c_VariableWrite, CuControlCode::mergeCode(0, 7, 0, 1, /*stall=*/0)),
            makeIns("MOV_R_R", {c_Pt, 7, 6}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        KernelControlCodes kcc = makeKcc(decoded);
        const ControlCodeCheckResult result = correctControlCodes(kcc, decoded, CuAsm::CuSMVersion(75));
        t.check("correctControlCodes() reports Corrected for two back-to-back adjacent VARIABLE producers",
                result.status == CheckStatus::Corrected);

        const CuControlCode producerA(kcc.ctrlCodeList[0]);
        const CuControlCode producerB(kcc.ctrlCodeList[1]);
        t.check("both genuinely-adjacent producers were actually assigned real scoreboard slots",
                producerA.getWriteSB() != -1 && producerB.getWriteSB() != -1);

        std::vector<DecodedInstruction> reChecked = decoded;
        for (std::size_t i = 0; i < reChecked.size(); ++i) {
            reChecked[i].ctrlCode = CuControlCode(kcc.ctrlCodeList[i]);
        }
        t.check("the back-to-back correction re-verifies clean", verifyControlCodes(reChecked, CuAsm::CuSMVersion(75)).status ==
                                                                       CheckStatus::Verified);
    }

    // ---- correctControlCodes(): regression for Reports/correct-cc-overconservative-barrier-
    // intervals.md -- many independent VARIABLE producers whose nearest consumer is far away (not
    // adjacent) must NOT be reported Unrepairable just because a naive count of barrier-eligible
    // producers exceeds the 6 physical slots. This is the exact shape of the bug: a real,
    // ptxas-compiled GEMM kernel with 28 such producers, all safely closed by natural instruction
    // spacing and needing zero scoreboard slots, was previously rejected as Unrepairable. ----
    {
        // Instructions 0..6: seven independent "UNCURATED_R_ARI" (deliberately not one of
        // minSafeBarrierMargin()'s curated keys -- exercises the uncurated default margin of 1)
        // producers on distinct registers, each followed by one padding instruction -- enough that
        // no producer is adjacent to the shared final consumer. Their *input* control codes have
        // no pre-existing barrier (writebar unset), so collectBarrierIntervals()'s safety net
        // (Reports/correct-cc-margin-heuristic-strips-real-barriers.md) doesn't force protection
        // either -- this exercises the margin skip alone. Old (buggy) behavior: all seven got real
        // intervals overlapping at the shared final consumer, exceeding the 6 physical slots --
        // Unrepairable. Fixed behavior: every one of them clears the default margin, so none need
        // a slot at all.
        std::vector<DecodedInstruction> decoded;
        for (int i = 0; i < 7; ++i) {
            decoded.push_back(makeIns("UNCURATED_R_ARI", {c_Pt, i}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                                       CuControlCode::mergeCode(0, 7, /*writebar=*/7, 1, /*stall=*/0)));
            decoded.push_back(makeIns("NOP", {c_Pt}, {}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)));
        }
        std::vector<std::int64_t> consumerVals{c_Pt};
        std::vector<OperandRoleEntry> consumerRoles;
        for (int i = 0; i < 7; ++i) {
            consumerVals.push_back(i);
            consumerRoles.push_back({OperandKind::GPR, AccessMode::READ});
        }
        decoded.push_back(makeIns("BAR_R_R_R_R_R_R_R", consumerVals, consumerRoles, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)));

        KernelControlCodes kcc = makeKcc(decoded);
        const ControlCodeCheckResult result = correctControlCodes(kcc, decoded, CuAsm::CuSMVersion(75));
        t.check("correctControlCodes() no longer reports Unrepairable for far-apart VARIABLE producers "
                "that natural instruction spacing already closes",
                result.status == CheckStatus::Corrected);
        t.check("...and needs zero scoreboard slots, since none of them are genuinely adjacent to their consumer",
                result.message.find("0/6") != std::string::npos);

        std::vector<DecodedInstruction> reChecked = decoded;
        for (std::size_t i = 0; i < reChecked.size(); ++i) {
            reChecked[i].ctrlCode = CuControlCode(kcc.ctrlCodeList[i]);
        }
        t.check("the zero-slot correction re-verifies clean",
                verifyControlCodes(reChecked, CuAsm::CuSMVersion(75)).status == CheckStatus::Verified);
    }

    // ---- correctControlCodes(): minSafeBarrierMargin() restores genuine multi-slot overlap
    // detection for two *different* curated opcodes, end to end -- the literal-adjacency-only
    // version of this fix could never produce two overlapping intervals (every triggered interval
    // was forced to exactly one instruction wide, and two distinct producers' width-1 windows can
    // never overlap each other). A per-opcode minimum margin above 1 removes that limitation: two
    // producers, each below their own curated margin threshold, can still need protection several
    // instructions out, and if those windows overlap, they still need distinct slots -- *because*
    // they're different insKeys (DMUL_R_R_R and MUFU_R_R); see assignBarrierSlots()'s own doc and
    // the dedicated same-insKey-sharing tests above/below for the complementary case
    // (Reports/correct-cc-shared-slot-reuse-not-modeled.md) where overlap alone is no longer
    // sufficient to force distinct slots. (LDS_R_ARI is deliberately not used here any more -- see
    // minSafeBarrierMargin()'s own doc for why its curated margin was proven unsound and removed.) ----
    {
        // 0: DMUL_R_R_R producer A on R4, margin 4 (< 48), no pre-existing barrier -- needs a real
        //    interval [0, 5) from margin logic alone (not the safety net).
        // 1: MUFU_R_R producer B on R6, margin 4 (< 15) -- needs a real interval [1, 6), which
        //    overlaps A's [0, 5) (A is still live when B's window opens). A different insKey from
        //    A, so the two must still get distinct slots despite the new same-insKey sharing rule.
        const std::vector<DecodedInstruction> decoded = {
            makeIns("DMUL_R_R_R", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, /*writebar=*/7, 1, /*stall=*/0)),
            makeIns("MUFU_R_R", {c_Pt, 6}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, /*writebar=*/7, 1, /*stall=*/0)),
            makeIns("MOV_R_II", {c_Pt, 10}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_II", {c_Pt, 11}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_II", {c_Pt, 12}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_R", {c_Pt, 20, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_R", {c_Pt, 21, 6}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto graph = buildHazardGraph(decoded);
        const auto intervals = collectBarrierIntervals(decoded, graph);
        t.check("both curated-margin producers get real intervals", intervals.size() == 2);
        t.check("...and those intervals genuinely overlap",
                intervals.size() == 2 && intervals[0].producerIndex == 0 && intervals[0].firstConsumerIndex == 5 &&
                    intervals[1].producerIndex == 1 && intervals[1].firstConsumerIndex == 6);

        KernelControlCodes kcc = makeKcc(decoded);
        const ControlCodeCheckResult result = correctControlCodes(kcc, decoded, CuAsm::CuSMVersion(75));
        t.check("correctControlCodes() reports Corrected for two overlapping curated-margin producers",
                result.status == CheckStatus::Corrected);

        const CuControlCode producerA(kcc.ctrlCodeList[0]);
        const CuControlCode producerB(kcc.ctrlCodeList[1]);
        t.check("...and assigns them distinct physical slots, since their windows genuinely overlap",
                producerA.getWriteSB() != -1 && producerB.getWriteSB() != -1 && producerA.getWriteSB() != producerB.getWriteSB());

        std::vector<DecodedInstruction> reChecked = decoded;
        for (std::size_t i = 0; i < reChecked.size(); ++i) {
            reChecked[i].ctrlCode = CuControlCode(kcc.ctrlCodeList[i]);
        }
        t.check("the distinct-slot correction re-verifies clean",
                verifyControlCodes(reChecked, CuAsm::CuSMVersion(75)).status == CheckStatus::Verified);
    }

    // ---- correctControlCodes(): regression for Reports/correct-cc-shared-slot-reuse-not-
    // modeled.md -- three overlapping VARIABLE producers of the *same* insKey must be packed into
    // one shared physical slot, not rejected as needing 3 distinct ones. This is the exact shape
    // of the bug: a real, ptxas-compiled GEMM kernel's `LDS.U` producers do exactly this (three
    // back-to-back same-opcode loads sharing one slot, closed by a single later wait), and the
    // old interval-coloring model (any overlap forces distinct slots) treated them as needing 3
    // separate slots, which multiplied across a densely unrolled loop until it exceeded the 6
    // physical slots and falsely reported Unrepairable on a kernel `verify-cc` confirms is
    // already 100% correct. ----
    {
        // 0: LDG_R_ARI producer P0 on R4 -- interval [0, 10) (huge curated margin of 616 means it
        //    always needs a real interval, regardless of natural spacing).
        // 1: LDG_R_ARI producer P1 on R6 -- interval [1, 5), overlapping P0's.
        // 2: LDG_R_ARI producer P2 on R8 -- interval [2, 8), overlapping both P0's and P1's.
        // 3-4, 6-7, 9: unrelated padding (distinct destination registers, never read/written by
        //    the producers or their consumers, so they add no extra hazard edges).
        // 5: consumer of P1 (RAW on R6) -- P1's *earliest* (and only) barrier-eligible consumer.
        // 8: consumer of P2 (RAW on R8).
        // 10: consumer of P0 (RAW on R4).
        const std::vector<DecodedInstruction> decoded = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, /*writebar=*/7, 1, /*stall=*/0)),
            makeIns("LDG_R_ARI", {c_Pt, 6}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, /*writebar=*/7, 1, /*stall=*/0)),
            makeIns("LDG_R_ARI", {c_Pt, 8}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, /*writebar=*/7, 1, /*stall=*/0)),
            makeIns("MOV_R_II", {c_Pt, 20}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_II", {c_Pt, 21}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_R", {c_Pt, 22, 6}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_II", {c_Pt, 23}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_II", {c_Pt, 24}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_R", {c_Pt, 25, 8}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_II", {c_Pt, 26}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_R", {c_Pt, 27, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto graph = buildHazardGraph(decoded);
        const auto intervals = collectBarrierIntervals(decoded, graph);
        t.check("all three same-insKey producers get real intervals", intervals.size() == 3);
        t.check("...and those intervals are mutually overlapping (the old model's worst case)",
                intervals.size() == 3 && intervals[0].producerIndex == 0 && intervals[0].firstConsumerIndex == 10 &&
                    intervals[1].producerIndex == 1 && intervals[1].firstConsumerIndex == 5 &&
                    intervals[2].producerIndex == 2 && intervals[2].firstConsumerIndex == 8);

        KernelControlCodes kcc = makeKcc(decoded);
        const ControlCodeCheckResult result = correctControlCodes(kcc, decoded, CuAsm::CuSMVersion(75));
        t.check("correctControlCodes() reports Corrected for three overlapping same-insKey producers "
                "(not Unrepairable)",
                result.status == CheckStatus::Corrected);
        t.check("...and packs all three into a single shared scoreboard slot", result.message.find("1/6") != std::string::npos);

        const CuControlCode producerP0(kcc.ctrlCodeList[0]);
        const CuControlCode producerP1(kcc.ctrlCodeList[1]);
        const CuControlCode producerP2(kcc.ctrlCodeList[2]);
        t.check("all three producers were actually assigned the same real scoreboard slot",
                producerP0.getWriteSB() != -1 && producerP0.getWriteSB() == producerP1.getWriteSB() &&
                    producerP0.getWriteSB() == producerP2.getWriteSB());

        std::vector<DecodedInstruction> reChecked = decoded;
        for (std::size_t i = 0; i < reChecked.size(); ++i) {
            reChecked[i].ctrlCode = CuControlCode(kcc.ctrlCodeList[i]);
        }
        t.check("the shared-slot correction re-verifies clean",
                verifyControlCodes(reChecked, CuAsm::CuSMVersion(75)).status == CheckStatus::Verified);
    }

    return t.finish("test_ccCorrect");
}
