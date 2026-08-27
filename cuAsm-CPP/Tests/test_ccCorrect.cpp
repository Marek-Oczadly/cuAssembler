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
    }

    // ---- collectBarrierIntervals() ----
    {
        // 0: VARIABLE:WRITE producer on R4.
        // 1: reads R4 (RAW, barrier-eligible) -- first consumer.
        // 2: reads R4 again -- must NOT create a second interval, only widen/ignore since 1 < 2.
        const std::vector<DecodedInstruction> instrs = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, 0, 1, 2)),
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

    // ---- assignBarrierSlots() ----
    {
        // Two non-overlapping intervals (touching at the boundary) may share a slot.
        const std::vector<BarrierInterval> intervals = {
            {0, 2, BarrierType::WRITE},
            {2, 4, BarrierType::WRITE},
        };
        const auto assignment = assignBarrierSlots(intervals);
        t.check("assignBarrierSlots() succeeds for two touching (non-overlapping) intervals", assignment.has_value());
        t.check("two touching intervals are packed into a single slot",
                assignment.has_value() && assignment->slotsUsed == 1);
    }
    {
        // Two genuinely overlapping intervals must get distinct slots.
        const std::vector<BarrierInterval> intervals = {
            {0, 3, BarrierType::WRITE},
            {1, 4, BarrierType::WRITE},
        };
        const auto assignment = assignBarrierSlots(intervals);
        t.check("assignBarrierSlots() succeeds for two overlapping intervals", assignment.has_value());
        t.check("two overlapping intervals need two distinct slots",
                assignment.has_value() && assignment->slotsUsed == 2 &&
                    assignment->slotOf.at(0) != assignment->slotOf.at(1));
    }
    {
        // 7 intervals all mutually overlapping (all start before a shared end point) exceed the
        // 6 physical scoreboard slots -- the "provably unrepairable in place" case.
        std::vector<BarrierInterval> intervals;
        for (std::size_t i = 0; i < static_cast<std::size_t>(c_ScoreboardSlotCount) + 1; ++i) {
            intervals.push_back(BarrierInterval{i, static_cast<std::size_t>(c_ScoreboardSlotCount) + 1, BarrierType::WRITE});
        }
        t.check("assignBarrierSlots() reports no valid assignment for more simultaneous intervals than physical slots",
                !assignBarrierSlots(intervals).has_value());
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

    // ---- correctControlCodes(): unrepairable (>6 simultaneously-live VARIABLE producers) ----
    {
        // Instructions 0..6: seven independent VARIABLE:WRITE producers on distinct registers.
        // Instruction 7: a single consumer reading all seven -- forces all seven barrier
        // intervals to overlap (each is [i, 7]), needing 7 distinct scoreboard slots.
        std::vector<DecodedInstruction> decoded;
        for (int i = 0; i < 7; ++i) {
            decoded.push_back(makeIns("LDG_R_ARI", {c_Pt, i}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                                       CuControlCode::mergeCode(0, 7, /*writebar=*/0, 1, 2)));
        }
        std::vector<std::int64_t> consumerVals{c_Pt};
        std::vector<OperandRoleEntry> consumerRoles;
        for (int i = 0; i < 7; ++i) {
            consumerVals.push_back(i);
            consumerRoles.push_back({OperandKind::GPR, AccessMode::READ});
        }
        decoded.push_back(makeIns("BAR_R_R_R_R_R_R_R", consumerVals, consumerRoles, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)));

        KernelControlCodes kcc = makeKcc(decoded);
        const std::vector<std::uint32_t> before = kcc.ctrlCodeList;
        const ControlCodeCheckResult result = correctControlCodes(kcc, decoded, CuAsm::CuSMVersion(75));
        t.check("correctControlCodes() reports Unrepairable when more than 6 producers are simultaneously live",
                result.status == CheckStatus::Unrepairable && !result.message.empty());
        t.check("kcc.ctrlCodeList is left completely unchanged after an Unrepairable result", kcc.ctrlCodeList == before);
    }

    return t.finish("test_ccCorrect");
}
