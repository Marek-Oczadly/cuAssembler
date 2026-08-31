#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/graph/adjacency_list.hpp>

#include "../CuAsm/CuControlCode.hpp"
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
using CuAsm::Tools::buildHazardGraph;
using CuAsm::Tools::CheckStatus;
using CuAsm::Tools::DecodedInstruction;
using CuAsm::Tools::HazardType;
using CuAsm::Tools::HazardViolation;
using CuAsm::Tools::registerAccessesOf;
using CuAsm::Tools::RegisterSpace;
using CuAsm::Tools::simulateAndVerify;
using CuAsm::Tools::verifyControlCodes;

namespace {

/// PT/UPT sentinel value (CuInsParser::parsePred()'s "unconditional" guard, and the raw value
/// "PT"/"UPT" tokens rewrite to) -- excluded from hazard tracking, see ccCommon.hpp's
/// c_PredTrueIndex.
constexpr std::int64_t c_Pt = 7;
/// RZ sentinel value ("RZ" rewrites to "R255") -- excluded from hazard tracking, see
/// ccCommon.hpp's c_GprZeroIndex.
constexpr std::int64_t c_Rz = 255;
/// URZ sentinel value ("URZ" rewrites to "UR63") -- excluded from hazard tracking, see
/// ccCommon.hpp's c_UgprZeroIndex. Deliberately distinct from c_Rz: GPR and UGPR are different
/// register files with different-numbered zero registers.
constexpr std::int64_t c_Urz = 63;

const LatencyClassEntry c_Fixed{LatencyKind::FIXED, std::nullopt};
const LatencyClassEntry c_VariableWrite{LatencyKind::VARIABLE, BarrierType::WRITE};
const LatencyClassEntry c_VariableRead{LatencyKind::VARIABLE, BarrierType::READ};

/**
 * @brief Builds a synthetic DecodedInstruction by hand, bypassing OperandRoleTable/
 *        LatencyClassTable/CuInsParser entirely -- this test exercises buildHazardGraph()/
 *        simulateAndVerify()/verifyControlCodes() directly against hand-built instruction
 *        sequences (Reports/tasks.md Phase 5's own guidance: "prefer synthetic instruction
 *        sequences for hazard/scoreboard-simulator unit tests"), so insKey only needs to be
 *        unique-ish for readability, not a real curated table key.
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

/**
 * @brief Same as makeIns(), but with an explicit address -- needed for the CFG-aware
 *        buildHazardGraph() tests below, since computeControlFlowSuccessors() resolves branch
 *        targets from real instruction addresses (makeIns() always hardcodes address=0, which is
 *        fine for the straight-line sequences the rest of this file uses, but not for anything
 *        containing a real branch).
 * @param address This instruction's address within its (synthetic) kernel.
 * @param insKey Label for this instruction.
 * @param insVals insVals[0] is the guard-predicate value; insVals[1..] align 1:1 with roles.
 * @param roles Per-operand roles, aligned with insVals[1..].
 * @param latency This instruction's latency classification.
 * @param ctrl Raw control code (see CuControlCode::mergeCode()).
 * @return The synthetic instruction.
 **/
DecodedInstruction makeInsAt(std::uint64_t address, std::string insKey, std::vector<std::int64_t> insVals,
                              std::vector<OperandRoleEntry> roles, LatencyClassEntry latency, std::uint32_t ctrl) {
    DecodedInstruction ins = makeIns(std::move(insKey), std::move(insVals), std::move(roles), latency, ctrl);
    ins.address = address;
    return ins;
}

/** @brief Finds a violation matching (producerIndex, consumerIndex) in a violation list, or nullptr. */
const HazardViolation* findViolation(const std::vector<HazardViolation>& violations, std::size_t producer, std::size_t consumer) {
    for (const auto& v : violations) {
        if (v.producerIndex == producer && v.consumerIndex == consumer) {
            return &v;
        }
    }
    return nullptr;
}

} // namespace

/**
 * @brief Exercises Reports/tasks.md Phase 3: the RAW/WAW/WAR hazard graph builder
 *        (buildHazardGraph(), backed by a Boost.Graph adjacency_list) and the 6-slot scoreboard
 *        simulator (simulateAndVerify()) that checks it against real control codes, plus the
 *        verifyControlCodes() wrapper both feed. All against hand-built synthetic instruction
 *        sequences -- no cuobjdump/real cubin needed (that path is test_ccCommon.cpp's job).
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    // ---- registerAccessesOf(): sentinel exclusion and guard-predicate resolution ----

    {
        // A plain-datapath instruction ("IADD3...", doesn't start with 'U') writing R4, reading
        // RZ (sentinel, excluded) and R5, guarded by PT (sentinel, excluded).
        const DecodedInstruction ins = makeIns("IADD3_R_R_R_R", {c_Pt, 4, c_Rz, 5, c_Rz},
                                                {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ},
                                                 {OperandKind::GPR, AccessMode::READ}, {OperandKind::GPR, AccessMode::READ}},
                                                c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1));
        const auto accesses = registerAccessesOf(ins);
        t.check("registerAccessesOf() excludes RZ operands and an unconditional (PT) guard, keeping only R4 (WRITE)/R5 (READ)",
                accesses.size() == 2);
        bool sawR4Write = false;
        bool sawR5Read = false;
        for (const auto& a : accesses) {
            if (a.space == RegisterSpace::GPR && a.number == 4 && a.mode == AccessMode::WRITE) {
                sawR4Write = true;
            }
            if (a.space == RegisterSpace::GPR && a.number == 5 && a.mode == AccessMode::READ) {
                sawR5Read = true;
            }
        }
        t.check("registerAccessesOf() reports R4 as WRITE and R5 as READ", sawR4Write && sawR5Read);
    }
    {
        // A uniform-datapath instruction ("UIADD3...") guarded by a *negated* predicate, "@!P2"
        // -- parsePred() encodes this as (2 + 8) = 10; masking with 0x7 must recover register 2,
        // and the opcode's 'U' prefix must select the UPRED register space, not PRED.
        const DecodedInstruction ins =
            makeIns("UIADD3_UR_UR_UR_UR", {10, c_Urz, c_Urz, c_Urz, c_Urz},
                    {{OperandKind::UGPR, AccessMode::WRITE}, {OperandKind::UGPR, AccessMode::READ},
                     {OperandKind::UGPR, AccessMode::READ}, {OperandKind::UGPR, AccessMode::READ}},
                    c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1));
        const auto accesses = registerAccessesOf(ins);
        t.check("registerAccessesOf() resolves a negated uniform guard (\"@!UP2\"-style value 10) to UPRED register 2, READ",
                accesses.size() == 1 && accesses[0].space == RegisterSpace::UPRED && accesses[0].number == 2 &&
                    accesses[0].mode == AccessMode::READ);
    }
    {
        // Duplicate accesses to the same register within one instruction (e.g. "IADD3 R4, R4,
        // R4, RZ") must merge into a single READ_WRITE entry, not three separate accesses.
        const DecodedInstruction ins = makeIns(
            "IADD3_R_R_R_R", {c_Pt, 4, 4, 4, c_Rz},
            {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}, {OperandKind::GPR, AccessMode::READ},
             {OperandKind::GPR, AccessMode::READ}},
            c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1));
        const auto accesses = registerAccessesOf(ins);
        t.check("registerAccessesOf() merges repeated same-register accesses into one READ_WRITE entry",
                accesses.size() == 1 && accesses[0].space == RegisterSpace::GPR && accesses[0].number == 4 &&
                    accesses[0].mode == AccessMode::READ_WRITE);
    }

    // ---- buildHazardGraph(): edge shape for a small RAW/WAW/WAR sequence ----
    {
        // 0: writes R4          (producer)
        // 1: reads  R4          -> RAW edge 0->1
        // 2: writes R4          -> WAW edge 0->2 (from the *original* writer, not instruction 1), WAR edge 1->2
        const std::vector<DecodedInstruction> instrs = {
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto graph = buildHazardGraph(instrs);
        t.checkEqual("buildHazardGraph() emits exactly 3 edges for a write/read/write chain on one register",
                     boost::num_edges(graph), std::size_t{3});

        const auto [e01, e01Found] = boost::edge(0, 1, graph);
        t.check("edge 0->1 exists and is RAW on R4", e01Found && graph[e01].type == HazardType::RAW &&
                                                          graph[e01].regSpace == RegisterSpace::GPR && graph[e01].regNumber == 4);
        const auto [e02, e02Found] = boost::edge(0, 2, graph);
        t.check("edge 0->2 exists and is WAW on R4 (from the original writer, not instruction 1)",
                e02Found && graph[e02].type == HazardType::WAW);
        const auto [e12, e12Found] = boost::edge(1, 2, graph);
        t.check("edge 1->2 exists and is WAR on R4", e12Found && graph[e12].type == HazardType::WAR);
    }

    // ---- simulateAndVerify(): RAW via a VARIABLE:WRITE barrier ----
    {
        // 0: VARIABLE:WRITE producer, opens scoreboard slot 0 (writebar=0).
        // 1: consumer reading R4, waits on slot 0 (waitbar bit 0 set) -> hazard correctly closed.
        const std::vector<DecodedInstruction> closed = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(/*waitbar=*/0, /*readbar=*/7, /*writebar=*/0, /*yieldFlag=*/1, /*stall=*/2)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(/*waitbar=*/0b000001, /*readbar=*/7, /*writebar=*/7, /*yieldFlag=*/1, /*stall=*/1)),
        };
        const auto violations = simulateAndVerify(closed, buildHazardGraph(closed));
        t.check("a VARIABLE:WRITE RAW hazard closed by the consumer's own wait produces no violation", violations.empty());
    }
    {
        // Same producer, but the consumer never waits on slot 0.
        const std::vector<DecodedInstruction> open = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, /*writebar=*/0, 1, 2)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(/*waitbar=*/0, 7, 7, 1, 1)),
        };
        const auto violations = simulateAndVerify(open, buildHazardGraph(open));
        t.check("an unclosed VARIABLE:WRITE RAW hazard is reported", violations.size() == 1);
        const HazardViolation* v = findViolation(violations, 0, 1);
        t.check("the reported violation is RAW on R4 from instruction 0 to 1",
                v != nullptr && v->type == HazardType::RAW && v->regSpace == RegisterSpace::GPR && v->regNumber == 4);
    }

    // ---- simulateAndVerify(): WAW, retired by an intervening instruction's wait ----
    {
        // 0: VARIABLE:WRITE producer on R4, opens slot 0, never itself waited on directly.
        // 1: unrelated instruction (touches R9) that happens to wait on slot 0 -> retires 0.
        // 2: writes R4 again (WAW edge 0->2) -- already retired by instruction 1's wait.
        const std::vector<DecodedInstruction> retiredEarly = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, /*writebar=*/0, 1, 2)),
            makeIns("MOV_R_II", {c_Pt, 9}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                    CuControlCode::mergeCode(/*waitbar=*/0b000001, 7, 7, 1, 1)),
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto violations = simulateAndVerify(retiredEarly, buildHazardGraph(retiredEarly));
        t.check("a WAW hazard retired by an earlier, unrelated instruction's wait (not the consumer's own) is closed",
                violations.empty());
    }

    // ---- simulateAndVerify(): WAR via a VARIABLE:READ barrier ----
    {
        // 0: VARIABLE:READ "store"-like op consuming R4 as its source, opens a READ barrier
        //    (readbar=1) protecting its still-in-flight read.
        // 1: writes R4 without waiting on slot 1 -> WAR hazard left open.
        const std::vector<DecodedInstruction> instrs = {
            makeIns("STG_ARI_R", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::READ}}, c_VariableRead,
                    CuControlCode::mergeCode(0, /*readbar=*/1, /*writebar=*/7, 1, 2)),
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                    CuControlCode::mergeCode(/*waitbar=*/0, 7, 7, 1, 1)),
        };
        const auto violations = simulateAndVerify(instrs, buildHazardGraph(instrs));
        t.check("an unclosed VARIABLE:READ WAR hazard is reported", violations.size() == 1);
        const HazardViolation* v = findViolation(violations, 0, 1);
        t.check("the reported violation is WAR on R4", v != nullptr && v->type == HazardType::WAR);
    }

    // ---- simulateAndVerify(): FIXED-latency conservative lower bound (adjacency + zero stall) ----
    {
        // Adjacent, zero stall: definitely wrong -- flagged.
        const std::vector<DecodedInstruction> adjacentZeroStall = {
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, /*stall=*/0)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        t.check("a FIXED-latency hazard with the producer immediately adjacent and stall=0 is flagged",
                simulateAndVerify(adjacentZeroStall, buildHazardGraph(adjacentZeroStall)).size() == 1);
    }
    {
        // Adjacent, nonzero stall: treated as satisfied (can't verify the exact cycle count, but
        // this is not the unconditionally-wrong case).
        const std::vector<DecodedInstruction> adjacentNonzeroStall = {
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, /*stall=*/1)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        t.check("a FIXED-latency hazard with the producer immediately adjacent but a nonzero stall is not flagged",
                simulateAndVerify(adjacentNonzeroStall, buildHazardGraph(adjacentNonzeroStall)).empty());
    }
    {
        // Not adjacent (an unrelated instruction in between), zero stall: not flagged -- any
        // instruction gap is treated as satisfying the (unknown, presumably small) fixed latency.
        const std::vector<DecodedInstruction> notAdjacent = {
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, /*stall=*/0)),
            makeIns("MOV_R_II", {c_Pt, 9}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        t.check("a FIXED-latency hazard with an intervening instruction (not immediately adjacent) is not flagged",
                simulateAndVerify(notAdjacent, buildHazardGraph(notAdjacent)).empty());
    }

    // ---- simulateAndVerify(): guard-predicate RAW hazard ----
    {
        // 0: writes P0 (VARIABLE:WRITE, opens slot 3).
        // 1: guarded by "@P0" (insVals[0] == 0), all its normal operands are RZ (sentinel) --
        //    the *only* real hazard here is the guard predicate itself. Doesn't wait on slot 3.
        const std::vector<DecodedInstruction> instrs = {
            makeIns("ISETP_P_R_R", {c_Pt, 0}, {{OperandKind::PRED, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, /*writebar=*/3, 1, 2)),
            makeIns("IADD3_R_R_R_R", {/*guard=*/0, c_Rz, c_Rz, c_Rz, c_Rz},
                    {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}, {OperandKind::GPR, AccessMode::READ},
                     {OperandKind::GPR, AccessMode::READ}},
                    c_Fixed, CuControlCode::mergeCode(/*waitbar=*/0, 7, 7, 1, 1)),
        };
        const auto violations = simulateAndVerify(instrs, buildHazardGraph(instrs));
        t.check("an unclosed hazard on a guard predicate (\"@P0\") is reported even though every named operand is RZ",
                violations.size() == 1);
        const HazardViolation* v = findViolation(violations, 0, 1);
        t.check("the reported guard-predicate violation is RAW on PRED0",
                v != nullptr && v->type == HazardType::RAW && v->regSpace == RegisterSpace::PRED && v->regNumber == 0);
    }
    {
        // Same producer, but the consumer is unconditional (guard == PT) -- no guard hazard at all.
        const std::vector<DecodedInstruction> instrs = {
            makeIns("ISETP_P_R_R", {c_Pt, 0}, {{OperandKind::PRED, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, 3, 1, 2)),
            makeIns("IADD3_R_R_R_R", {c_Pt, c_Rz, c_Rz, c_Rz, c_Rz},
                    {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}, {OperandKind::GPR, AccessMode::READ},
                     {OperandKind::GPR, AccessMode::READ}},
                    c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        t.check("a PT (unconditional) guard never creates a hazard edge, even with an unrelated P0 producer nearby",
                boost::num_edges(buildHazardGraph(instrs)) == 0);
    }

    // ---- simulateAndVerify(): same-slot reuse before a wait retires every pending producer (FIFO), not just the latest ----
    {
        // 0: VARIABLE:WRITE on R4, opens slot 0. Never itself directly waited on.
        // 1: VARIABLE:WRITE on R6, *also* opens slot 0, with no wait in between -- real hardware
        //    retires same-slot producers in issue order, so a later wait on slot 0 must retire
        //    both, not just the most recent (this is the exact real-world pattern confirmed
        //    against the TestData/CuTest/cudatest.7.sm_75.cubin fixture: consecutive `S2R ...,
        //    W0` reads closed by a single later wait).
        // 2: reads both R4 (RAW edge from 0) and R6 (RAW edge from 1), waits on slot 0 once --
        //    must retire both producers, leaving no violation.
        const std::vector<DecodedInstruction> instrs = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, /*writebar=*/0, 1, 2)),
            makeIns("LDG_R_ARI", {c_Pt, 6}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, /*writebar=*/0, 1, 2)),
            makeIns("IADD3_R_R_R_R", {c_Pt, 8, 4, 6, c_Rz},
                    {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}, {OperandKind::GPR, AccessMode::READ},
                     {OperandKind::GPR, AccessMode::READ}},
                    c_Fixed, CuControlCode::mergeCode(/*waitbar=*/0b000001, 7, 7, 1, 1)),
        };
        const auto violations = simulateAndVerify(instrs, buildHazardGraph(instrs));
        t.check("a single wait on a reused scoreboard slot retires every producer that pended on it, not just the latest",
                violations.empty());
    }

    // ---- simulateAndVerify(): a VARIABLE producer's *other* (non-barrier-protected) access uses the FIXED-adjacency rule ----
    {
        // A synthetic "LDG R5, [R4]"-shaped producer: VARIABLE:WRITE (protects the async-written
        // R5), but its address-register read of R4 is a synchronous, at-issue read -- not
        // protected by the write barrier at all (confirmed against the real fixture: an
        // address-register WAR hazard is never closed by the producer's own write barrier).
        // 0: reads R4 (address) and writes R5, VARIABLE:WRITE, opens slot 0 -- irrelevant to R4.
        // 1: immediately adjacent, overwrites R4, stall=0 -- WAR hazard on R4, unconditionally
        //    wrong regardless of instruction 0's write barrier (which protects R5, not R4).
        const std::vector<DecodedInstruction> adjacentZeroStall = {
            makeIns("LDG_R_ARI", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::R_ADDR, AccessMode::READ}},
                    c_VariableWrite, CuControlCode::mergeCode(0, 7, /*writebar=*/0, 1, /*stall=*/0)),
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto violations = simulateAndVerify(adjacentZeroStall, buildHazardGraph(adjacentZeroStall));
        t.check("a WAR hazard on a VARIABLE:WRITE producer's *address-register* read is not treated as write-barrier-protected",
                violations.size() == 1 && findViolation(violations, 0, 1) != nullptr &&
                    findViolation(violations, 0, 1)->type == HazardType::WAR);

        // Same shape, but with a stall so the FIXED-adjacency rule is satisfied -- confirms this
        // is genuinely using the FIXED-style check, not just always flagging address-register WAR.
        const std::vector<DecodedInstruction> adjacentNonzeroStall = {
            makeIns("LDG_R_ARI", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::R_ADDR, AccessMode::READ}},
                    c_VariableWrite, CuControlCode::mergeCode(0, 7, /*writebar=*/0, 1, /*stall=*/1)),
            makeIns("MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        t.check("...but is not flagged once the producer's stall count is nonzero, same as an ordinary FIXED-latency hazard",
                simulateAndVerify(adjacentNonzeroStall, buildHazardGraph(adjacentNonzeroStall)).empty());
    }

    // ---- buildHazardGraph(): CFG-aware reconvergence -- two mutually-exclusive branch legs that
    // each write the same register must NOT interfere with each other, but both must correctly
    // reach a shared reconvergence-point reader (Reports/tasks.md's 2026-08-30 CFG-blindness fix,
    // §3's "New gotchas" item -- this is the exact shape that fix targets). ----
    {
        // 0 (0x00): "@P0 BRA 0x30;"        -- conditional, succ = {1 (fallthrough), 3 (target)}.
        // 1 (0x10): writes R4               -- the "P0 false" leg.
        // 2 (0x20): "BRA 0x40;"             -- unconditional, jumps over instruction 3 to reconverge.
        // 3 (0x30): writes R4               -- the "P0 true" leg, reached directly from instruction 0.
        // 4 (0x40): reads R4                -- reconvergence point; must see BOTH 1 and 3 as reaching
        //           writers, but 1 and 3 must never see each other (they can never both execute on
        //           the same thread's path).
        const std::vector<DecodedInstruction> instrs = {
            makeInsAt(0x00, "BRA_II", {0, 0x20}, {}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeInsAt(0x10, "MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                      CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeInsAt(0x20, "BRA_II", {c_Pt, 0x10}, {}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeInsAt(0x30, "MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                      CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeInsAt(0x40, "MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}},
                      c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto graph = buildHazardGraph(instrs);
        t.checkEqual("a diamond CFG's reconvergence point gets exactly 2 RAW edges (one per sibling leg), no more",
                     boost::num_edges(graph), std::size_t{2});
        const auto [e14, e14Found] = boost::edge(1, 4, graph);
        t.check("edge 1->4 (the \"P0 false\" leg's write) exists and is RAW on R4",
                e14Found && graph[e14].type == HazardType::RAW && graph[e14].regNumber == 4);
        const auto [e34, e34Found] = boost::edge(3, 4, graph);
        t.check("edge 3->4 (the \"P0 true\" leg's write) exists and is RAW on R4",
                e34Found && graph[e34].type == HazardType::RAW && graph[e34].regNumber == 4);
        const auto [e13, e13Found] = boost::edge(1, 3, graph);
        const auto [e31, e31Found] = boost::edge(3, 1, graph);
        t.check("the two sibling legs (1 and 3) have no edge between them in either direction -- they "
                "can never both execute on the same thread's path",
                !e13Found && !e31Found);
    }

    // ---- buildHazardGraph(): a loop's back-edge is deliberately dropped from the reaching-
    // definitions dataflow (Reports/tasks.md's documented, still-open "loop-carried hazards are not
    // modeled" limitation) -- this locks in that documented behavior so a future change to this
    // rule is a deliberate decision, not an accidental regression. ----
    {
        // 0 (0x00): reads R4               -- the loop's top; on a *real* second iteration this
        //           would depend on instruction 1's write from the *previous* iteration, but that
        //           dependency can only be reached via instruction 2's backward branch.
        // 1 (0x10): writes R4.
        // 2 (0x20): "BRA 0x00;"            -- unconditional loop-back edge (target 0 <= source 2).
        const std::vector<DecodedInstruction> instrs = {
            makeInsAt(0x00, "MOV_R_R", {c_Pt, 9, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}},
                      c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeInsAt(0x10, "MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                      CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeInsAt(0x20, "BRA_II", {c_Pt, -0x30}, {}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        const auto graph = buildHazardGraph(instrs);
        t.checkEqual("instruction 0 (the loop top) has no incoming hazard edges at all -- the only "
                     "thing that could reach it (the backward branch) is deliberately excluded",
                     boost::in_degree(0, graph), std::size_t{0});
        const auto [e10, e10Found] = boost::edge(1, 0, graph);
        t.check("no RAW edge exists from instruction 1's write back to instruction 0's read across "
                "the (dropped) loop back-edge",
                !e10Found);
    }

    // ---- buildHazardGraph(): a dead (@!PT) branch with an unresolvable target must not corrupt or
    // crash the graph builder -- computeControlFlowSuccessors() never attempts to resolve a target
    // it will never take (see test_ccControlFlow.cpp for the focused version of this check; this
    // confirms buildHazardGraph() built on top of it inherits the same safety). ----
    {
        const std::vector<DecodedInstruction> instrs = {
            makeInsAt(0x00, "BRA_II", {15, 0x7fffffff}, {}, c_Fixed, CuControlCode::mergeCode(0, 7, 7, 1, 1)),
            makeInsAt(0x10, "MOV_R_II", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_Fixed,
                      CuControlCode::mergeCode(0, 7, 7, 1, 1)),
        };
        CuAsm::Tools::HazardGraph graph;
        t.checkNoThrow("buildHazardGraph() tolerates a dead (@!PT) branch with an unresolvable target",
                        [&]() { graph = buildHazardGraph(instrs); });
        t.checkEqual("...and produces no hazard edges for this 2-instruction sequence (no real dependency exists)",
                     boost::num_edges(graph), std::size_t{0});
    }

    // ---- verifyControlCodes(): the public wrapper ----
    {
        const std::vector<DecodedInstruction> verified = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, 0, 1, 2)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(0b000001, 7, 7, 1, 1)),
        };
        const auto result = verifyControlCodes(verified, CuAsm::CuSMVersion(75));
        t.check("verifyControlCodes() reports Verified with no violations for a correctly-guarded sequence",
                result.status == CheckStatus::Verified && result.violations.empty() && !result.message.empty());
    }
    {
        const std::vector<DecodedInstruction> violated = {
            makeIns("LDG_R_ARI", {c_Pt, 4}, {{OperandKind::GPR, AccessMode::WRITE}}, c_VariableWrite,
                    CuControlCode::mergeCode(0, 7, 0, 1, 2)),
            makeIns("MOV_R_R", {c_Pt, 5, 4}, {{OperandKind::GPR, AccessMode::WRITE}, {OperandKind::GPR, AccessMode::READ}}, c_Fixed,
                    CuControlCode::mergeCode(/*waitbar=*/0, 7, 7, 1, 1)),
        };
        const auto result = verifyControlCodes(violated, CuAsm::CuSMVersion(75));
        t.check("verifyControlCodes() reports Violated with the offending edge for an incorrectly-guarded sequence",
                result.status == CheckStatus::Violated && result.violations.size() == 1 && !result.message.empty());
    }

    return t.finish("test_ccHazard");
}
