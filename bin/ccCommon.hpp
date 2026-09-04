#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/sequential_vertex_coloring.hpp>
#include <elfio/elfio.hpp>

#include "../CuAsm/CuControlCode.hpp"
#include "../CuAsm/CuInsFeeder.hpp"
#include "../CuAsm/CuInsParser.hpp"
#include "../CuAsm/CuSMVersion.hpp"
#include "../CuAsm/LatencyClass.hpp"
#include "../CuAsm/OperandRole.hpp"
#include "../CuAsm/common.hpp"
#include "../CuAsm/utils/BigNum.hpp"
#include "../CuAsm/utils/CubinUtils.hpp"

// Shared helpers for the verify-cc/correct-cc control-code tools: loading a cubin's per-kernel
// control-code/instruction-code lists, decoding those instructions into role/latency-annotated
// records (decodeInstructions(), Reports/tasks.md Phase 2), building/simulating the resulting
// RAW/WAW/WAR hazard graph (buildHazardGraph()/simulateAndVerify(), Phase 3), and the
// verification/correction entry points verify-cc/correct-cc call. Both verifyControlCodes() and
// correctControlCodes() (Phase 4: barrier-slot interval-coloring allocation, stall-count
// recomputation, and a reject/rollback path for provably-unrepairable-in-place reorders) are now
// real implementations -- see Reports/control-codes-validation.md and Reports/tasks.md for the
// design this was built against.

namespace CuAsm::Tools {

/// One kernel's ".text.<kernel>" section, decoded into parallel control-code/instruction-code
/// lists via CuSMVersion::splitCtrlCodeFromBytes.
struct KernelControlCodes {
    std::string kernelName;
    /// Byte offset of this kernel's ".text.<kernel>" section within the cubin file.
    std::uint64_t sectionOffset = 0;
    /// Byte size of that section, used to sanity-check that a corrected kernel re-merges to
    /// exactly the same size (correction only ever rewrites existing control-code fields, never
    /// adds/removes instructions).
    std::uint64_t sectionSize = 0;
    std::vector<std::uint32_t> ctrlCodeList;
    std::vector<BigInt> insCodeList;
};

/// Which physical register file a hazard-relevant operand belongs to, grouping OperandKind
/// values that share a register file: OperandKind::R_ADDR (the register component of a
/// [Rn+...] memory address) reads the same file as OperandKind::GPR, and OperandKind::UR_ADDR
/// reads the same file as OperandKind::UGPR -- an address-computation register is a real
/// register-file read for hazard purposes, it just happens to feed an address unit instead of
/// an ALU (Reports/tasks.md Phase 3's "address-register operands generate hazards on the
/// address register itself" scoping note). Non-register OperandKinds (immediates, memory-address
/// immediate/bank components, barrier/scoreboard/special-register operands) have no RegisterSpace
/// and are never part of a hazard edge -- see registerSpaceOf().
enum class RegisterSpace {
    GPR,
    UGPR,
    PRED,
    UPRED,
};

/**
 * @brief Formats a RegisterSpace for diagnostic messages (e.g. verify-cc's violation printout).
 * @param space Value to format.
 * @return "GPR", "UGPR", "PRED", or "UPRED".
 **/
inline std::string toString(RegisterSpace space) {
    switch (space) {
    case RegisterSpace::GPR:
        return "GPR";
    case RegisterSpace::UGPR:
        return "UGPR";
    case RegisterSpace::PRED:
        return "PRED";
    case RegisterSpace::UPRED:
        return "UPRED";
    }
    return "?";
}

/// The kind of true/output/anti dependency a hazard-graph edge represents, mirroring the
/// standard compiler-dependence-analysis vocabulary: RAW (read-after-write, a true dependency --
/// the consumer reads a value the producer wrote), WAW (write-after-write, an output dependency
/// -- both instructions write the same register, and completion order matters), WAR
/// (write-after-read, an anti-dependency -- the consumer overwrites a register the producer was
/// still reading).
enum class HazardType {
    RAW,
    WAW,
    WAR,
};

/**
 * @brief Formats a HazardType for diagnostic messages.
 * @param type Value to format.
 * @return "RAW", "WAW", or "WAR".
 **/
inline std::string toString(HazardType type) {
    switch (type) {
    case HazardType::RAW:
        return "RAW";
    case HazardType::WAW:
        return "WAW";
    case HazardType::WAR:
        return "WAR";
    }
    return "?";
}

/// One hazard edge (Reports/tasks.md Phase 3) that the scoreboard simulator found is not
/// correctly closed by the current control codes: producerIndex's access to the named register
/// is not guaranteed complete/visible by the time consumerIndex issues, either because a
/// VARIABLE-latency producer's barrier was never waited on, or because a FIXED-latency producer
/// immediately precedes its consumer with a zero stall count (see simulateAndVerify()'s doc for
/// why FIXED-latency checking is a conservative lower bound rather than an exact cycle model --
/// no per-opcode numeric latency table exists anywhere in this codebase, only the boolean
/// FIXED/VARIABLE classification LatencyClassTable curates).
struct HazardViolation {
    /// Index (within the kernel's instruction list) of the instruction whose register access
    /// created this hazard.
    std::size_t producerIndex = 0;
    /// Index of the instruction whose register access was not correctly guarded against
    /// producerIndex's.
    std::size_t consumerIndex = 0;
    /// RAW/WAW/WAR classification of this hazard.
    HazardType type = HazardType::RAW;
    /// Register file the conflicting access is in.
    RegisterSpace regSpace = RegisterSpace::GPR;
    /// Register number within regSpace.
    int regNumber = 0;
    /// Human-readable explanation of why this edge is considered unresolved.
    std::string reason;
};

/// Outcome of a verification or correction pass.
enum class CheckStatus {
    NotImplemented,
    /// Every RAW/WAW/WAR hazard edge found in the kernel's instructions is correctly closed by
    /// its current control codes. verifyControlCodes()-only.
    Verified,
    /// At least one hazard edge is not correctly closed -- see ControlCodeCheckResult::violations.
    /// verifyControlCodes()-only.
    Violated,
    /// correctControlCodes() recomputed every control code and every hazard this model can detect
    /// is now closed -- kcc.ctrlCodeList was rewritten in place. correctControlCodes()-only.
    Corrected,
    /// correctControlCodes() could not find a valid assignment of VARIABLE-latency producers to
    /// the 6 physical scoreboard slots for this instruction order (Reports/tasks.md Phase 4's
    /// "reorder is provably unrepairable in place" case) -- kcc.ctrlCodeList was left completely
    /// unchanged. correctControlCodes()-only.
    Unrepairable,
};

/// Result of running verifyControlCodes()/correctControlCodes() against one kernel.
struct ControlCodeCheckResult {
    CheckStatus status = CheckStatus::NotImplemented;
    std::string message;
    /// Populated (non-empty) when status == CheckStatus::Violated, and in the rare
    /// CheckStatus::Unrepairable case where correctControlCodes()'s own post-correction
    /// self-check finds residual violations (an internal modeling bug, not the expected
    /// barrier-coloring-rejection reason -- see correctControlCodes()'s doc); empty otherwise.
    std::vector<HazardViolation> violations;
};

/// Minimum SM version verify-cc/correct-cc support: Turing (sm_75) and newer. Earlier
/// architectures are excluded not because CuSMVersion can't decode/assemble them, but because
/// the operand read/write-role and instruction-latency data the eventual hazard-analysis work
/// needs (see Reports/control-codes-validation.md) is most reliably documented from Turing
/// onward -- restricting this tool's scope here keeps that future data-curation work on solid
/// ground instead of guessing at Volta/Pascal/Maxwell behavior from thinner documentation.
inline constexpr int c_MinSupportedSMVersion = 75;

/**
 * @brief Detects a cubin's target architecture from its ELF header flags, mirroring
 *        CubinFile::loadCubin's own "smVersion = e_flags & 0xff" derivation.
 * @param ef Already-loaded ELF reader for the cubin.
 * @return The detected CuSMVersion, or std::nullopt if the encoded SM version isn't one
 *         CuSMVersion accepts (see CuSMVersion::validSMVersions()), or is older than
 *         c_MinSupportedSMVersion.
 **/
inline std::optional<CuSMVersion> detectArch(const ELFIO::elfio& ef) {
    const auto smVersion = static_cast<int>(ef.get_flags() & 0xff);
    if (smVersion < c_MinSupportedSMVersion) {
        return std::nullopt;
    }
    try {
        return CuSMVersion(smVersion);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

/**
 * @brief Decodes every ".text.<kernel>" section of a cubin into its per-kernel control-code/
 *        instruction-code lists.
 * @param ef Already-loaded ELF reader for the cubin.
 * @param arch The cubin's architecture, as returned by detectArch().
 * @return One KernelControlCodes entry per ".text.<kernel>" section found, in section order.
 **/
inline std::vector<KernelControlCodes> loadControlCodes(const ELFIO::elfio& ef, const CuSMVersion& arch) {
    static constexpr std::string_view textPrefix = ".text.";

    std::vector<KernelControlCodes> result;
    for (const auto& sec : ef.sections) {
        const std::string& name = sec->get_name();
        if (!name.starts_with(textPrefix)) {
            continue;
        }

        const auto* data = reinterpret_cast<const std::byte*>(sec->get_data());
        const std::span<const std::byte> bytes(data, sec->get_size());

        KernelControlCodes kcc;
        kcc.kernelName = name.substr(textPrefix.size());
        kcc.sectionOffset = sec->get_offset();
        kcc.sectionSize = sec->get_size();
        std::tie(kcc.ctrlCodeList, kcc.insCodeList) = arch.splitCtrlCodeFromBytes(bytes);
        result.push_back(std::move(kcc));
    }
    return result;
}

/// One instruction fully decoded for hazard analysis: its parsed operand shape (from
/// CuInsParser), its already-split control code, its byte address within the kernel, and the
/// curated per-operand roles (OperandRoleTable, Phase 0 of Reports/tasks.md) and latency
/// classification (LatencyClassTable, Phase 1) for its InsKey. Building one of these is the only
/// place those two tables and CuInsParser's output come together; the Phase 3 hazard graph/
/// scoreboard simulator walks vectors of these instead of re-deriving any of it from raw bytes.
struct DecodedInstruction {
    /// Address of this instruction within its ".text.<kernel>" section, mirroring
    /// CuSMVersion::getInsOffsetFromIndex() for this instruction's index within the kernel.
    std::uint64_t address = 0;
    /// Opcode + operand-shape key, e.g. "FFMA_R_R_R_R" (CuInsParser::parse()'s first return value).
    std::string insKey;
    /// Per-operand values; insVals[CuInsParser::PRED_VAL_IDX] is always the guard-predicate value.
    std::vector<std::int64_t> insVals;
    /// Modifier list for this instance, consulted by OperandRoleTable::lookup() to select overrides.
    std::vector<std::string> insModi;
    /// This instruction's already-split control code (waitbar/readbar/writebar/yield/stall).
    CuControlCode ctrlCode;
    /// Curated per-operand roles for this instance (OperandRoleTable::lookup()'s result), aligned
    /// with insVals/insModi from CuInsParser::OPERAND_VAL_IDX onward (guard predicate excluded).
    std::vector<OperandRoleEntry> roles;
    /// Curated latency classification for insKey (LatencyClassTable::lookup()'s result).
    LatencyClassEntry latency;
};

/// A register operand this instruction actually reads and/or writes, resolved to a concrete
/// (RegisterSpace, number) pair -- what the Phase 3 hazard graph builder walks per instruction,
/// built from a DecodedInstruction's roles/insVals plus its (implicit, never role-tabled) guard
/// predicate. See registerAccessesOf().
struct RegisterAccess {
    RegisterSpace space;
    int number;
    AccessMode mode;
};

/// GPR index CuInsParser::constTr() rewrites "RZ" to (CuInsParser.cpp's `rzRe()` substitution):
/// the architectural zero register. Reading it always yields 0 and writing it is discarded, so it
/// never carries a real cross-instruction dependency and is excluded from hazard-edge generation.
inline constexpr int c_GprZeroIndex = 255;
/// UGPR index "URZ" rewrites to -- URZ's uniform-datapath equivalent of c_GprZeroIndex.
inline constexpr int c_UgprZeroIndex = 63;
/// PRED/UPRED index "PT"/"UPT" rewrite to, and also the sentinel CuInsParser::parsePred() returns
/// for an omitted guard predicate (Reports/tasks.md gotcha: PRED_VAL_IDX's "7 = unconditional").
/// The always-true constant predicate wire, not a real predicate register -- excluded from hazard
/// edges the same way c_GprZeroIndex/c_UgprZeroIndex are. A negated guard ("@!P0") is encoded as
/// (register + 8) by parsePred(); masking with 0x7 before comparing against this sentinel recovers
/// the real register number regardless of negation (see registerAccessesOf()'s guard-predicate
/// handling), and "@!PT" (value 15, "never execute") masks down to this same sentinel too -- it is
/// a constant-false guard, not a real register dependency either.
inline constexpr int c_PredTrueIndex = 7;

/**
 * @brief Maps an OperandKind to the physical register file it reads/writes, for OperandKinds that
 *        participate in hazard-edge generation at all.
 * @param kind Operand kind to classify.
 * @return The RegisterSpace kind shares a register file with (R_ADDR groups with GPR, UR_ADDR
 *         groups with UGPR -- Reports/tasks.md Phase 3's "address-register operands generate
 *         hazards on the address register itself" scoping decision), or std::nullopt for every
 *         other OperandKind (immediates, memory-address immediate/bank components, barrier/
 *         scoreboard/special-register operands -- none of these are general-purpose/uniform/
 *         predicate register-file accesses, so none of them can alias a GPR/UR/PRED/UPRED
 *         producer's write; deliberately out of scope, see Phase 3's checklist).
 **/
inline std::optional<RegisterSpace> registerSpaceOf(OperandKind kind) {
    switch (kind) {
    case OperandKind::GPR:
    case OperandKind::R_ADDR:
        return RegisterSpace::GPR;
    case OperandKind::UGPR:
    case OperandKind::UR_ADDR:
        return RegisterSpace::UGPR;
    case OperandKind::PRED:
        return RegisterSpace::PRED;
    case OperandKind::UPRED:
        return RegisterSpace::UPRED;
    default:
        return std::nullopt;
    }
}

/**
 * @brief Checks whether (space, number) names an architectural sentinel register (RZ/URZ/PT/UPT)
 *        rather than a real, stateful register -- see c_GprZeroIndex/c_UgprZeroIndex/c_PredTrueIndex.
 * @param space Register file to check within.
 * @param number Register number within space.
 * @return True if this is a sentinel that never carries a real cross-instruction dependency.
 **/
inline bool isSentinelRegister(RegisterSpace space, int number) {
    switch (space) {
    case RegisterSpace::GPR:
        return number == c_GprZeroIndex;
    case RegisterSpace::UGPR:
        return number == c_UgprZeroIndex;
    case RegisterSpace::PRED:
    case RegisterSpace::UPRED:
        return number == c_PredTrueIndex;
    }
    return false;
}

/// How a control-flow instruction's guard predicate resolves statically, for CFG-successor
/// purposes (see computeControlFlowSuccessors()): "PT"/no explicit guard is always taken,
/// "@!PT" is a compiled-in-but-dead branch that never taken, and a real predicate register is a
/// genuine runtime condition -- either outcome must be treated as reachable.
enum class GuardKind {
    AlwaysTrue,
    AlwaysFalse,
    Conditional,
};

/**
 * @brief Classifies an instruction's guard predicate as always-taken, never-taken, or a genuine
 *        runtime condition, for control-flow-successor purposes -- distinct from
 *        isSentinelRegister()'s register-file-hazard-tracking concern, since here the negation bit
 *        parsePred() folds into "register + 8" (isSentinelRegister()/registerAccessesOf() mask it
 *        away because @P.Guard and @!P.Guard are the same *register* dependency either way) matters:
 *        @PT and @!PT are both "PT" the register, but opposite constants.
 * @param ins Instruction to classify.
 * @return GuardKind::AlwaysTrue for an unguarded/"@PT" instruction, GuardKind::AlwaysFalse for
 *         "@!PT", GuardKind::Conditional for a real predicate register (negated or not).
 **/
inline GuardKind guardKindOf(const DecodedInstruction& ins) {
    const std::int64_t raw = ins.insVals[CuInsParser::PRED_VAL_IDX];
    const bool negated = (raw & 0x8) != 0;
    const int base = static_cast<int>(raw) & 0x7;
    if (base != c_PredTrueIndex) {
        return GuardKind::Conditional;
    }
    return negated ? GuardKind::AlwaysFalse : GuardKind::AlwaysTrue;
}

/**
 * @brief Computes each instruction's real control-flow successor set within its own kernel, for
 *        buildHazardGraph()'s reachability-aware dataflow (Reports/tasks.md Phase 5's dated
 *        "CFG-blindness" note -- this is the fix for it): every instruction either falls through
 *        to the next one, jumps to a resolved target, both (a conditional branch), or neither (a
 *        guaranteed thread exit), following real SASS control-flow semantics rather than assuming
 *        flat program order:
 *        - `CALL`/`CAL`: always falls through only. A call always eventually returns to the next
 *          instruction, and the callee's own body is a *different* kernel's/function's
 *          KernelControlCodes entry entirely (never part of this vertex set), so its target --
 *          however it's encoded -- is irrelevant to this CFG.
 *        - `BSSY`/`SSY`/`PBK`/`PRET`: convergence-stack bookkeeping (reconvergence/break/return
 *          target for the hardware's divergence stack), not an actual control transfer -- always
 *          falls through, target operand unused.
 *        - `EXIT`/`RET`: terminates this thread's path here (`RET` returns to a caller outside
 *          this vertex set) -- never has an in-kernel jump target to resolve; contributes a
 *          fallthrough successor only when its guard isn't GuardKind::AlwaysTrue (a guaranteed
 *          exit has no successor at all).
 *        - `BRA`/`JMP` with a resolvable immediate target (InsKey ends "_II", per
 *          CuInsParser::specialTreatment()'s relative-offset encoding for addrFuncs): target
 *          successor unless GuardKind::AlwaysFalse, fallthrough successor unless
 *          GuardKind::AlwaysTrue -- i.e. both for a real conditional branch. An ".ABS" modifier
 *          (insModi contains "0_ABS", e.g. `BRA.ABS 0x...;`) means specialTreatment() left the
 *          immediate as a raw absolute address instead of converting it to a PC-relative offset
 *          (see its own `m_InsOpFull.find("ABS")` check) -- this function must mirror that and use
 *          the immediate directly as the target rather than adding address+instructionLength to
 *          it, or it silently computes the wrong target address (previously unhandled: this
 *          function used the relative-offset formula unconditionally).
 *        - Anything else that still needs a target it can't resolve (`BRX`/`BRXU`/`JMX`/`JMXU`
 *          computed jump tables, or a `BRA`/`JMP` whose target isn't a plain immediate) is a hard
 *          error, not a guess -- out of scope per this codebase's existing curation philosophy;
 *          none of the 23 `Tests/CheckControlCodes` fixtures exercise one (confirmed by scanning
 *          their real `cuobjdump -sass` output, Reports/tasks.md's dated note).
 *        - Every other opcode: falls through only.
 * @param instrs Kernel's decoded instructions, in program order.
 * @return successors[i] is the list of instruction indices i's control flow can reach next
 *         (0, 1, or 2 entries; order between fallthrough/target is not significant -- only the set
 *         of successors matters to the reaching-definitions dataflow that consumes this).
 * @throws std::runtime_error for an indirect/unresolvable branch target, or if a resolved target
 *         address doesn't land exactly on one of instrs' own instruction addresses.
 **/
inline std::vector<std::vector<std::size_t>> computeControlFlowSuccessors(const std::vector<DecodedInstruction>& instrs) {
    std::vector<std::vector<std::size_t>> succ(instrs.size());

    std::map<std::uint64_t, std::size_t> addrToIndex;
    std::optional<std::int64_t> instructionLength;
    auto ensureAddrIndex = [&]() {
        if (addrToIndex.empty()) {
            for (std::size_t k = 0; k < instrs.size(); ++k) {
                addrToIndex.emplace(instrs[k].address, k);
            }
        }
    };
    auto ensureInstructionLength = [&]() -> std::int64_t {
        if (!instructionLength.has_value()) {
            if (instrs.size() < 2) {
                throw std::runtime_error(
                    "computeControlFlowSuccessors: cannot resolve a branch target with fewer than 2 instructions "
                    "to infer the instruction stride from");
            }
            const std::int64_t stride = static_cast<std::int64_t>(instrs[1].address) - static_cast<std::int64_t>(instrs[0].address);
            if (stride <= 0) {
                throw std::runtime_error("computeControlFlowSuccessors: non-positive inferred instruction stride");
            }
            instructionLength = stride;
        }
        return *instructionLength;
    };

    for (std::size_t i = 0; i < instrs.size(); ++i) {
        const std::string opcode = instrs[i].insKey.substr(0, instrs[i].insKey.find('_'));
        const bool hasFallthrough = i + 1 < instrs.size();

        if (opcode == "CALL" || opcode == "CAL" || opcode == "BSSY" || opcode == "SSY" || opcode == "PBK" || opcode == "PRET") {
            if (hasFallthrough) {
                succ[i].push_back(i + 1);
            }
            continue;
        }

        if (opcode == "EXIT" || opcode == "RET") {
            if (guardKindOf(instrs[i]) != GuardKind::AlwaysTrue && hasFallthrough) {
                succ[i].push_back(i + 1);
            }
            continue;
        }

        if (opcode == "BRA" || opcode == "JMP" || opcode == "BRX" || opcode == "BRXU" || opcode == "JMX" || opcode == "JMXU") {
            const bool resolvable =
                (opcode == "BRA" || opcode == "JMP") && instrs[i].insKey.ends_with("_II") && !instrs[i].insVals.empty();
            if (!resolvable) {
                throw std::runtime_error("computeControlFlowSuccessors: instruction " + std::to_string(i) + " (\"" +
                                          instrs[i].insKey +
                                          "\") is an indirect/computed branch whose target cannot be statically "
                                          "resolved -- out of scope, see this function's doc");
            }

            const bool isAbsolute =
                std::find(instrs[i].insModi.begin(), instrs[i].insModi.end(), "0_ABS") != instrs[i].insModi.end();

            const GuardKind guard = guardKindOf(instrs[i]);
            if (guard != GuardKind::AlwaysFalse) {
                std::int64_t target = instrs[i].insVals.back();
                if (!isAbsolute) {
                    const std::int64_t len = ensureInstructionLength();
                    target += static_cast<std::int64_t>(instrs[i].address) + len;
                }
                ensureAddrIndex();
                const auto it = target >= 0 ? addrToIndex.find(static_cast<std::uint64_t>(target)) : addrToIndex.end();
                if (it == addrToIndex.end()) {
                    throw std::runtime_error("computeControlFlowSuccessors: branch at instruction " + std::to_string(i) +
                                              " targets an address outside this kernel's instruction list");
                }
                succ[i].push_back(it->second);
            }
            if (guard != GuardKind::AlwaysTrue && hasFallthrough) {
                succ[i].push_back(i + 1);
            }
            continue;
        }

        if (hasFallthrough) {
            succ[i].push_back(i + 1);
        }
    }

    return succ;
}

/**
 * @brief Resolves one decoded instruction's real register-file accesses: its curated per-operand
 *        roles (OperandRoleTable, restricted to register-class OperandKinds via
 *        registerSpaceOf()) plus its guard predicate, which OperandRoleTable deliberately never
 *        tables (every instruction carries it implicitly -- OperandRole.hpp's OperandKind doc) but
 *        which is a completely ordinary READ dependency for hazard purposes: `@P1 IADD3 ...`
 *        genuinely must wait for whatever last wrote P1. Sentinel registers (RZ/URZ/PT/UPT) are
 *        dropped -- they never carry a real dependency (see isSentinelRegister()). Duplicate
 *        accesses to the same register within one instruction (e.g. `IADD3 R4, R4, R4, RZ`, or a
 *        role list with a READ and a WRITE role on the same register number) are merged into one
 *        AccessMode::READ_WRITE entry so buildHazardGraph() adds exactly one edge per real
 *        register, not one per occurrence.
 * @param ins Instruction to resolve accesses for.
 * @return This instruction's real register-file accesses, one per distinct (space, number).
 **/
inline std::vector<RegisterAccess> registerAccessesOf(const DecodedInstruction& ins) {
    std::map<std::pair<RegisterSpace, int>, AccessMode> merged;

    // Guard predicate: same register file as the instruction's own datapath (uniform-datapath
    // opcodes, i.e. those spelled "U..." -- CuInsParser::parsePred()'s own rule for the
    // no-explicit-guard case, mirrored here since an explicit "@UP0"/"@P0" guard's parsed value
    // carries no space tag of its own to read back -- guard with UP, everything else with P).
    const std::string opcode = ins.insKey.substr(0, ins.insKey.find('_'));
    const RegisterSpace guardSpace = (!opcode.empty() && opcode.front() == 'U') ? RegisterSpace::UPRED : RegisterSpace::PRED;
    const int guardReg = static_cast<int>(ins.insVals[CuInsParser::PRED_VAL_IDX]) & 0x7;
    if (!isSentinelRegister(guardSpace, guardReg)) {
        merged.emplace(std::make_pair(guardSpace, guardReg), AccessMode::READ);
    }

    for (std::size_t k = 0; k < ins.roles.size(); ++k) {
        const std::optional<RegisterSpace> space = registerSpaceOf(ins.roles[k].kind);
        if (!space.has_value()) {
            continue;
        }
        const int regNum = static_cast<int>(ins.insVals[CuInsParser::OPERAND_VAL_IDX + k]);
        if (isSentinelRegister(*space, regNum)) {
            continue;
        }

        const auto key = std::make_pair(*space, regNum);
        const auto [it, inserted] = merged.try_emplace(key, ins.roles[k].mode);
        if (!inserted && it->second != ins.roles[k].mode) {
            it->second = AccessMode::READ_WRITE;
        }
    }

    std::vector<RegisterAccess> result;
    result.reserve(merged.size());
    for (const auto& [key, mode] : merged) {
        result.push_back(RegisterAccess{key.first, key.second, mode});
    }
    return result;
}

/// Per-edge annotation for HazardGraph: which register the edge's hazard is about, and its
/// RAW/WAW/WAR classification.
struct HazardEdgeProperties {
    HazardType type = HazardType::RAW;
    RegisterSpace regSpace = RegisterSpace::GPR;
    int regNumber = 0;
};

/// Directed hazard graph over one kernel's instructions: vertex i is instruction i (0-based index
/// into the DecodedInstruction vector buildHazardGraph() was built from), and an edge p->c means
/// instruction p's register access must be visible/complete before instruction c's -- exactly the
/// RAW/WAW/WAR dependency simulateAndVerify() checks is correctly closed by the current control
/// codes. Built with Boost.Graph per CLAUDE.md's steer toward it for this exact dependency-graph
/// problem (see test_nvinfo.cpp for this codebase's existing adjacency_list usage precedent) --
/// vecS/vecS vertex/edge storage since vertices are already a dense 0..N-1 index space (no name
/// map needed, unlike test_nvinfo's string-keyed graph). bidirectionalS, not plain directedS:
/// simulateAndVerify() walks in_edges() per consumer, which plain directedS's adjacency_list
/// specialization does not provide (no reverse-edge bookkeeping) -- bidirectionalS adds it.
using HazardGraph = boost::adjacency_list<boost::vecS, boost::vecS, boost::bidirectionalS, boost::no_property, HazardEdgeProperties>;

/**
 * @brief Builds the RAW/WAW/WAR hazard graph for one kernel's decoded instructions: a
 *        reaching-definitions/reaching-reads dataflow analysis over the kernel's real,
 *        *forward-only* control-flow graph (computeControlFlowSuccessors(), with any backward
 *        edge dropped -- see below), not a flat program-order scan -- an edge p->c exists only
 *        when some real per-thread execution path can actually reach c from p with no intervening
 *        same-register write between them, mirroring standard compiler dependence-analysis/
 *        instruction-scheduling-legality construction.
 *
 *        FIXED 2026-08-30 (was Reports/tasks.md Phase 5's dated "CFG-blindness" gap, confirmed via
 *        Tests/CheckControlCodes fixtures 2026-08-28): the previous version linked every register
 *        access to whatever the *previous* accessor in raw index order was, with no notion of
 *        branches at all -- unsound across a divergent, BSSY/BSYNC-bounded region where multiple
 *        mutually-exclusive branch legs each access the same register and reconverge at a shared
 *        label, since it produced a spurious edge between two accesses that can never actually
 *        execute on the same thread's path (simulateAndVerify() then correctly found real,
 *        working ptxas control codes don't protect a dependency that isn't real). This version's
 *        edges are per-real-path reachability instead: for each register, forward dataflow over
 *        computeControlFlowSuccessors()'s CFG tracks, at the start of every instruction, the *set*
 *        of writers (respectively: readers since the most recent possible writer) that can reach
 *        that point along some path without being killed by an intervening write -- generalizing
 *        the old single "last writer"/"readers since write" scalars to sets that merge (union) at
 *        every CFG join point, which is exactly what makes mutually-exclusive branch legs stop
 *        interfering: two writers on sibling legs never appear in the same instruction's reaching
 *        set unless a real path can reach one from the other. A write kills all pending reaching
 *        writers/readers for its register at that instruction (its own write becomes the new sole
 *        reaching writer, with no readers yet) -- the same "reset" semantics the old per-register
 *        history had, just applied per-CFG-node instead of per-linear-index.
 *
 *        Backward edges (a branch whose target index is <= its own index -- i.e. a real loop's
 *        back-edge, confirmed distinct from the forward-only sibling-reconvergence case: an
 *        earlier version of this fix propagated across *every* CFG edge including these, which
 *        correctly found genuine loop-carried WAW/RAW/WAR self-edges on `Tests/CheckControlCodes`
 *        kernels with real loops -- `SharedMemory`/`MatrixMultiply`/`LocalMemorySpill` -- but
 *        simulateAndVerify() only ever walks instrs *once*, top to bottom, so it has no way to
 *        check an edge whose "producer" is later in program order than its own "consumer": by the
 *        time the simulated scoreboard state reaches the producer's index, the consumer's check
 *        already ran. That is a second, genuinely different problem from CFG-blindness (checking a
 *        loop-carried hazard needs simulateAndVerify() itself to model at least one virtual repeat
 *        of the loop body, not just a better hazard graph) -- out of scope here, and not a
 *        regression, since the *old* flat linear-index scan could never produce a backward edge
 *        either (its "last writer so far" state is by construction always an earlier index than
 *        the instruction currently being processed). So: only a strictly-forward CFG edge
 *        (successor index > predecessor index) is followed by this dataflow at all, which as a
 *        pleasant side effect also makes the reaching sets a plain topological (index-ascending)
 *        one-pass computation -- no fixed-point iteration needed, since every predecessor a node
 *        can have (post-filtering) is guaranteed already finalized by the time that node is
 *        reached.
 * @param instrs Kernel's decoded instructions, in program order.
 * @return The hazard graph: instrs.size() vertices (vertex i == instrs[i]), one edge per real
 *         RAW/WAW/WAR dependency found along a forward execution path.
 * @throws std::runtime_error See computeControlFlowSuccessors().
 **/
inline HazardGraph buildHazardGraph(const std::vector<DecodedInstruction>& instrs) {
    HazardGraph graph(instrs.size());
    if (instrs.empty()) {
        return graph;
    }

    const std::vector<std::vector<std::size_t>> succ = computeControlFlowSuccessors(instrs);
    std::vector<std::vector<std::size_t>> pred(instrs.size());
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        for (const std::size_t s : succ[i]) {
            // Forward-only: see this function's doc for why a backward (loop) edge is
            // deliberately excluded from the reaching-definitions dataflow below.
            if (s > i) {
                pred[s].push_back(i);
            }
        }
    }

    std::vector<std::vector<RegisterAccess>> accessesOf(instrs.size());
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        accessesOf[i] = registerAccessesOf(instrs[i]);
    }

    using RegKey = std::pair<RegisterSpace, int>;
    using RegSetMap = std::map<RegKey, std::set<std::size_t>>;

    auto unionInto = [](RegSetMap& dst, const RegSetMap& src) {
        for (const auto& [key, srcSet] : src) {
            dst[key].insert(srcSet.begin(), srcSet.end());
        }
    };

    std::vector<RegSetMap> writeIn(instrs.size()), writeOut(instrs.size());
    std::vector<RegSetMap> readIn(instrs.size()), readOut(instrs.size());

    // A single index-ascending pass suffices: pred[i] only ever contains indices < i (forward
    // edges only, see above), so every predecessor's writeOut/readOut is already final by the
    // time instruction i is processed -- this is a topological-order walk, not an approximation.
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        for (const std::size_t p : pred[i]) {
            unionInto(writeIn[i], writeOut[p]);
            unionInto(readIn[i], readOut[p]);
        }

        RegSetMap newWriteOut = writeIn[i];
        RegSetMap newReadOut = readIn[i];
        for (const RegisterAccess& acc : accessesOf[i]) {
            const RegKey key{acc.space, acc.number};
            if (acc.mode == AccessMode::WRITE || acc.mode == AccessMode::READ_WRITE) {
                // This instruction's write is now the sole reaching writer for key, and resets
                // the reaching-reads-since-write set to empty (mirroring the old per-register
                // history's "h.readersSinceWrite.clear()" on every write).
                newWriteOut[key] = {i};
                newReadOut[key].clear();
            } else {
                newReadOut[key].insert(i);
            }
        }
        writeOut[i] = std::move(newWriteOut);
        readOut[i] = std::move(newReadOut);
    }

    for (std::size_t i = 0; i < instrs.size(); ++i) {
        for (const RegisterAccess& acc : accessesOf[i]) {
            const RegKey key{acc.space, acc.number};
            if (acc.mode == AccessMode::READ || acc.mode == AccessMode::READ_WRITE) {
                if (const auto it = writeIn[i].find(key); it != writeIn[i].end()) {
                    for (const std::size_t producer : it->second) {
                        boost::add_edge(producer, i, HazardEdgeProperties{HazardType::RAW, acc.space, acc.number}, graph);
                    }
                }
            }
            if (acc.mode == AccessMode::WRITE || acc.mode == AccessMode::READ_WRITE) {
                if (const auto it = writeIn[i].find(key); it != writeIn[i].end()) {
                    for (const std::size_t producer : it->second) {
                        boost::add_edge(producer, i, HazardEdgeProperties{HazardType::WAW, acc.space, acc.number}, graph);
                    }
                }
                if (const auto it = readIn[i].find(key); it != readIn[i].end()) {
                    for (const std::size_t reader : it->second) {
                        boost::add_edge(reader, i, HazardEdgeProperties{HazardType::WAR, acc.space, acc.number}, graph);
                    }
                }
            }
        }
    }

    return graph;
}

/// Number of physical scoreboard slots on Turing/Ampere (Reports/tasks.md gotcha 4: waitbar is a
/// 6-bit mask, one bit per slot, and getReadSB()/getWriteSB() range over the same 6 ids). Not
/// currently exposed as a queryable CuSMVersion constant (see Reports/tasks.md's Phase 3
/// reference notes) and, per the report's own scope, not expected to differ across this tool's
/// Turing/Ampere target range -- revisit if a later architecture changes physical slot capacity.
inline constexpr int c_ScoreboardSlotCount = 6;

/**
 * @brief Simulates the 6-slot scoreboard over one kernel's instructions in program order and
 *        checks every hazard-graph edge against it, i.e. the actual verification check (Reports/
 *        tasks.md Phase 3's checklist items 3-4). At each instruction i: first, every barrier id
 *        in i's own wait mask (CuControlCode::getBarrierSet()) retires every VARIABLE-latency
 *        producer currently pending on that slot -- their async ops are now guaranteed complete,
 *        matching real hardware (an instruction never issues until its own waits are satisfied).
 *        Then, every hazard edge incoming to i is checked against that now-updated state: a
 *        VARIABLE-latency producer's edge is closed iff it has been retired (by i's own wait, or
 *        by any earlier instruction's); a FIXED-latency producer's edge is closed unless i is the
 *        very next instruction after it with a zero stall count (see below for why this, and not
 *        an exact cycle count, is what gets checked). Finally, if i is itself a VARIABLE-latency
 *        producer, it joins the pending queue of whichever scoreboard slot its own control code
 *        names (getReadSB()/getWriteSB(), per LatencyClassEntry::barrier).
 *
 *        Same-slot reuse before a wait is a FIFO queue, not last-writer-wins (confirmed
 *        empirically against the real TestData/CuTest/cudatest.7.sm_75.cubin fixture -- see
 *        Reports/tasks.md's Phase 3 notes): real, working compiler output routinely assigns the
 *        *same* barrier id to several VARIABLE producers back-to-back with no wait between them
 *        (e.g. two consecutive `S2R ..., W0` reads of different special registers), then closes
 *        all of them with a single later wait on that one id. This is only sound because ops
 *        sharing one physical scoreboard slot retire in the order they were issued -- so waiting
 *        on the slot's *current* occupant transitively guarantees every earlier, still-pending
 *        occupant of the same slot is also done. An earlier version of this function modeled slot
 *        ownership as a single scalar (last claimant wins, "displacing" any earlier one, which can
 *        never retire again) -- correct for the *closing* half, but wrong here: it produced
 *        false-positive violations on exactly this real, working pattern. Modeled here as one
 *        pending-producer queue per slot: claiming a slot appends to it; waiting on a slot retires
 *        every producer currently in it (not just the most recent) and empties it.
 *
 *        Per-edge barrier relevance (confirmed empirically against the real
 *        TestData/CuTest/cudatest.7.sm_75.cubin fixture -- see Reports/tasks.md's Phase 3 notes):
 *        a VARIABLE producer's barrier protects only the *specific* access LatencyClassEntry::barrier
 *        names -- BarrierType::WRITE protects its destination write (so it's relevant to RAW/WAW
 *        edges, where the producer's role was a write), BarrierType::READ protects its still-in-
 *        flight source read (relevant to WAR edges, where the producer's role was a read). A
 *        producer's *other* access, if it has one, is synchronous/immediate regardless of the
 *        instruction's own VARIABLE classification -- e.g. `LDG.E.SYS R5, [R4]` is VARIABLE:WRITE
 *        (protects the async-written R5), but its address-register read of R4 is resolved at issue
 *        time like any ALU operand read, so a WAR hazard on R4 (some later instruction overwriting
 *        it) is checked via the same FIXED-latency rule as an ordinary producer, not via R5's write
 *        barrier -- confirmed directly: the fixture's real, correctly-scheduled control codes never
 *        protect an address-register WAR hazard with a wait, only genuine destination-write RAW/WAW
 *        hazards. Treating every edge from a VARIABLE producer as barrier-protected (an earlier,
 *        incorrect version of this function) produced false-positive violations on this real,
 *        working fixture for exactly this reason.
 *
 *        A VARIABLE classification is per-*opcode*; whether a given *instance* needs a barrier can
 *        still be a per-instance compiler choice (Reports/tasks.md Phase 1.1 already documents
 *        this for LDG/LDC/LDL, and it recurs here: a real, working sm_75 `LDS.U R6, [R6.X4] ;`
 *        instance -- LDS_R_ARI, curated VARIABLE:WRITE -- sets no write barrier at all, presumably
 *        because shared memory's latency is small/bounded enough that ptxas judged plain
 *        instruction spacing sufficient this time). So barrierProtectsThisAccess also requires the
 *        producer's control code to have *actually* opened the relevant barrier field
 *        (getReadSB()/getWriteSB() != -1) -- an instance that didn't is treated as FIXED-like for
 *        that access, falling through to the same conservative adjacency check below, rather than
 *        being assumed permanently unretirable.
 *
 *        FIXED-latency limitation (confirmed with the user before implementing, not decided
 *        unilaterally -- see Reports/tasks.md): no per-opcode numeric cycle-latency table exists
 *        anywhere in this codebase (LatencyClassTable, Phase 1, only curates the boolean
 *        FIXED/VARIABLE classification), so the exact number of stall cycles a FIXED-latency
 *        dependency needs cannot be computed. The check implemented here is a conservative lower
 *        bound: it only catches the unconditionally-wrong case (a true dependency made adjacent
 *        with zero stall between producer and consumer), and treats every other FIXED-latency (or
 *        FIXED-for-this-specific-access, per the paragraph above) edge as satisfied. A reorder that
 *        closes the distance to, say, 1 cycle when 2 were actually required would not be caught.
 *        Revisit this once a real per-opcode latency table exists (see Reports/tasks.md Phase 4's
 *        own stall-recomputation notes, which assume one).
 * @param instrs Kernel's decoded instructions, in program order -- must be the same vector (or an
 *        identically-ordered copy) buildHazardGraph() built graph from.
 * @param graph Hazard graph for instrs, as returned by buildHazardGraph(instrs).
 * @return Every hazard edge found not to be correctly closed; empty if the control codes correctly
 *         close every hazard this model can check.
 **/
inline std::vector<HazardViolation> simulateAndVerify(const std::vector<DecodedInstruction>& instrs, const HazardGraph& graph) {
    std::vector<HazardViolation> violations;

    std::array<std::vector<std::size_t>, c_ScoreboardSlotCount> slotPending{};
    std::vector<bool> retired(instrs.size(), false);

    for (std::size_t i = 0; i < instrs.size(); ++i) {
        // Step 1: this instruction's own wait mask retires *every* producer currently pending on
        // each named slot (not just the most recent -- see this function's doc for why same-slot
        // reuse before a wait is a FIFO queue, not last-writer-wins), since their async ops are
        // guaranteed complete before i's own reads/writes happen.
        for (const int slot : instrs[i].ctrlCode.getBarrierSet()) {
            if (slot < 0 || slot >= c_ScoreboardSlotCount) {
                continue;
            }
            std::vector<std::size_t>& pending = slotPending[static_cast<std::size_t>(slot)];
            for (const std::size_t producer : pending) {
                retired[producer] = true;
            }
            pending.clear();
        }

        // Step 2: check every hazard edge incoming to i against the state as of just before i
        // issues (i.e. including i's own wait, applied above, but nothing i itself opens below).
        const auto [inBegin, inEnd] = boost::in_edges(i, graph);
        for (auto ei = inBegin; ei != inEnd; ++ei) {
            const std::size_t producer = boost::source(*ei, graph);
            const HazardEdgeProperties& props = graph[*ei];
            const LatencyClassEntry& producerLatency = instrs[producer].latency;

            // Which of the producer's accesses this specific edge is about: RAW/WAW edges exist
            // because the producer *wrote* the register; WAR edges exist because it *read* it.
            const BarrierType relevantAccess = (props.type == HazardType::WAR) ? BarrierType::READ : BarrierType::WRITE;
            // A LatencyClassEntry's barrier type is a per-*opcode* classification, but Reports/
            // tasks.md Phase 1.1 already documents (LDG/LDC/LDL/LDS among others) that whether a
            // given *instance* actually needs a barrier is sometimes a per-instance compiler
            // scheduling choice, not a fixed property of the opcode -- confirmed again here
            // empirically (a real, working sm_75 LDS.U instance sets no barrier at all). So this
            // producer's barrier only protects the edge if it actually opened one for the
            // relevant access this time; if the control code shows the corresponding field unset,
            // that specific instance is behaving like a FIXED producer for this access, and falls
            // through to the same conservative FIXED-adjacency check below.
            const bool producerActuallyOpensBarrier = (relevantAccess == BarrierType::WRITE)
                                                           ? instrs[producer].ctrlCode.getWriteSB() != -1
                                                           : instrs[producer].ctrlCode.getReadSB() != -1;
            const bool barrierProtectsThisAccess = producerLatency.kind == LatencyKind::VARIABLE &&
                                                    *producerLatency.barrier == relevantAccess && producerActuallyOpensBarrier;

            bool closed = true;
            std::string reason;
            if (barrierProtectsThisAccess) {
                closed = retired[producer];
                if (!closed) {
                    reason = "producer's " + toString(*producerLatency.barrier) +
                             " scoreboard barrier was never waited on before this consumer issued";
                }
            } else {
                const bool adjacent = (i == producer + 1);
                const bool zeroStall = instrs[producer].ctrlCode.getStallCount() == 0;
                closed = !(adjacent && zeroStall);
                if (!closed) {
                    reason = "producer's relevant access to this register is not scoreboard-barrier-protected "
                             "(either FIXED latency, or a VARIABLE op whose barrier protects its other access) "
                             "and it immediately precedes this consumer with a stall count of 0";
                }
            }

            if (!closed) {
                violations.push_back(HazardViolation{producer, i, props.type, props.regSpace, props.regNumber, reason});
            }
        }

        // Step 3: if i is itself a VARIABLE-latency producer, it joins the pending queue of the
        // slot its control code names (appending, not replacing -- see this function's doc).
        if (instrs[i].latency.kind == LatencyKind::VARIABLE) {
            const int slot =
                (*instrs[i].latency.barrier == BarrierType::WRITE) ? instrs[i].ctrlCode.getWriteSB() : instrs[i].ctrlCode.getReadSB();
            if (slot >= 0 && slot < c_ScoreboardSlotCount) {
                slotPending[static_cast<std::size_t>(slot)].push_back(i);
            }
        }
    }

    return violations;
}

/**
 * @brief Runs `cuobjdump -sass` on a cubin file, applying the SM8x+ cache-policy desc-bit hack
 *        first if arch needs it, mirroring dsass.cpp's own cubin-dumping path (Reports/tasks.md
 *        gotcha 6: there is no native bytes->text SASS decoder in this codebase, and cuobjdump has
 *        no in-memory mode, so this is a real runtime dependency on the CUDA toolkit's cuobjdump
 *        being on PATH). Split out from decodeInstructions() so tests can exercise the pure
 *        sass-parsing half (decodeInstructionsFromSass()) against synthetic disassembly text,
 *        without a cuobjdump binary or cubin file on disk.
 * @param cubinPath Path of the cubin file to dump.
 * @param arch Cubin's architecture, as returned by detectArch().
 * @return The raw `cuobjdump -sass` output text.
 * @throws CuAsm::CalledProcessError if cuobjdump exits with a non-zero status.
 **/
inline std::string dumpSassForDecode(const std::string& cubinPath, const CuSMVersion& arch) {
    std::string binname = cubinPath;
    std::string tmpname;
    bool doDescHack = false;

    if (arch.needsDescHack()) {
        tmpname = CuAsm::getTempFileName("", "cuasm", "cubin");
        doDescHack = CuAsm::fixCubinDesc(cubinPath, tmpname);
        if (doDescHack) {
            binname = tmpname;
        }
    }

    std::string sass;
    try {
        sass = CuAsm::checkOutput({"cuobjdump", "-sass", binname}, /*mergeStderr=*/false);
    } catch (...) {
        if (doDescHack) {
            std::filesystem::remove(tmpname);
        }
        throw;
    }
    if (doDescHack) {
        std::filesystem::remove(tmpname);
    }

    return sass;
}

/**
 * @brief Parses `cuobjdump -sass` output text (as returned by dumpSassForDecode()) into role/
 *        latency-annotated DecodedInstruction records for a set of already-loaded kernels, aligned
 *        1:1 with each KernelControlCodes::ctrlCodeList/insCodeList. Pure text-in/struct-out --
 *        no subprocess or filesystem access -- so unit tests can exercise it directly with
 *        hand-written sass text (see Reports/tasks.md gotcha 7: the input must match cuobjdump's
 *        exact output format, since CuInsFeeder relies on it).
 *
 *        Instructions are matched to their KernelControlCodes entry by CuInsFeeder::CurrFuncName,
 *        which cuobjdump's "Function : <name>" lines set to exactly the ".text.<name>" section
 *        suffix loadControlCodes() already used for KernelControlCodes::kernelName -- real
 *        cuobjdump output has no ".section .text.<name>" lines of its own (only ".dsass"-style
 *        output, produced by CuInsFeeder::trans(), would), so FuncName is the only line type this
 *        matching can rely on.
 * @param sass Disassembly text, in exactly the format `cuobjdump -sass` produces.
 * @param kernels Per-kernel control-code lists, as already produced by loadControlCodes(ef, arch)
 *        for the same cubin sass was dumped from.
 * @param arch Cubin's architecture.
 * @return Map from kernel name to its decoded instruction list, each aligned 1:1 with the
 *         corresponding entry of kernels.
 * @throws std::runtime_error if a kernel in kernels has no matching "Function :" block in sass, if
 *         its instruction count doesn't match its ctrlCodeList/insCodeList, or if a disassembled
 *         instruction's address doesn't match its expected index-derived offset (misalignment
 *         between sass and the already-split control-code/instruction-code lists).
 * @throws std::out_of_range if any decoded instruction's InsKey has no curated entry in the
 *         OperandRoleTable/LatencyClassTable for arch -- by design (see those tables' lookup()
 *         docs): an unrecognized opcode must be a hard decode failure, never a best-effort guess.
 **/
inline std::map<std::string, std::vector<DecodedInstruction>>
decodeInstructionsFromSass(const std::string& sass, const std::vector<KernelControlCodes>& kernels, const CuSMVersion& arch) {
    std::map<std::string, std::vector<CuInsRecord>> byKernel;
    {
        std::istringstream sio(sass);
        CuInsFeeder feeder(sio);
        while (std::optional<CuInsRecord> rec = feeder.next()) {
            byKernel[feeder.CurrFuncName].push_back(std::move(*rec));
        }
    }

    OperandRoleTable& roleTable = OperandRoleTable::getStaticTable(arch.getVersionNumber());
    LatencyClassTable& latencyTable = LatencyClassTable::getStaticTable(arch.getVersionNumber());
    CuInsParser parser("sm_" + std::to_string(arch.getVersionNumber()));

    std::map<std::string, std::vector<DecodedInstruction>> result;
    for (const KernelControlCodes& kcc : kernels) {
        const auto it = byKernel.find(kcc.kernelName);
        if (it == byKernel.end()) {
            throw std::runtime_error("decodeInstructions: kernel \"" + kcc.kernelName +
                                      "\" has no matching \"Function :\" block in the sass disassembly!");
        }
        const std::vector<CuInsRecord>& records = it->second;
        if (records.size() != kcc.ctrlCodeList.size()) {
            throw std::runtime_error("decodeInstructions: kernel \"" + kcc.kernelName + "\" disassembled to " +
                                      std::to_string(records.size()) +
                                      " instructions, but its control-code list has " +
                                      std::to_string(kcc.ctrlCodeList.size()) + "!");
        }

        std::vector<DecodedInstruction> decoded;
        decoded.reserve(records.size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            const std::uint64_t addr = arch.getInsOffsetFromIndex(static_cast<int>(i));
            if (records[i].addr != addr) {
                std::ostringstream oss;
                oss << "decodeInstructions: kernel \"" << kcc.kernelName << "\" instruction " << i
                    << " address mismatch: sass showed 0x" << std::hex << records[i].addr << ", expected 0x" << addr
                    << "!";
                throw std::runtime_error(oss.str());
            }

            auto [insKey, insVals, insModi] = parser.parse(records[i].asmText, addr, kcc.insCodeList[i]);

            decoded.push_back(DecodedInstruction{
                .address = addr,
                .insKey = insKey,
                .insVals = insVals,
                .insModi = insModi,
                .ctrlCode = CuControlCode(kcc.ctrlCodeList[i]),
                .roles = roleTable.lookup(insKey, insModi),
                .latency = latencyTable.lookup(insKey),
            });
        }
        result.emplace(kcc.kernelName, std::move(decoded));
    }

    return result;
}

/**
 * @brief Decodes every kernel's instructions in a cubin file into role/latency-annotated
 *        DecodedInstruction records, aligned 1:1 with each KernelControlCodes::ctrlCodeList/
 *        insCodeList already produced by loadControlCodes(): dumpSassForDecode() +
 *        decodeInstructionsFromSass(), run once for the whole file rather than once per kernel
 *        (cuobjdump -sass always dumps every kernel in one call).
 * @param cubinPath Path of the cubin file on disk that kernels was built from -- loadControlCodes()
 *        only keeps an in-memory ELFIO reader, but cuobjdump needs a real file to disassemble.
 * @param kernels Per-kernel control-code lists, as already produced by loadControlCodes(ef, arch)
 *        for the same cubin.
 * @param arch Cubin's architecture, as returned by detectArch().
 * @return Map from kernel name to its decoded instruction list, each aligned 1:1 with the
 *         corresponding entry of kernels.
 * @throws CuAsm::CalledProcessError if cuobjdump exits with a non-zero status.
 * @throws std::runtime_error, std::out_of_range See decodeInstructionsFromSass().
 **/
inline std::map<std::string, std::vector<DecodedInstruction>>
decodeInstructions(const std::string& cubinPath, const std::vector<KernelControlCodes>& kernels, const CuSMVersion& arch) {
    if (kernels.empty()) {
        return {};
    }
    return decodeInstructionsFromSass(dumpSassForDecode(cubinPath, arch), kernels, arch);
}

/**
 * @brief Runs real hazard-based control-code verification (Reports/tasks.md Phase 3): builds the
 *        RAW/WAW/WAR hazard graph for a kernel's decoded instructions (buildHazardGraph()) and
 *        checks every edge against a 6-slot scoreboard simulation of the kernel's *current*
 *        control codes (simulateAndVerify()) -- i.e. whether every dependency that exists in this
 *        instruction order is still correctly guarded by the control codes already attached to it.
 *        See simulateAndVerify()'s doc for the one known limitation this carries (FIXED-latency
 *        hazards are checked against a conservative lower bound, not an exact cycle count, since
 *        no per-opcode latency table exists in this codebase).
 * @param decoded Kernel's instructions, already decoded via decodeInstructions()/
 *        decodeInstructionsFromSass() -- role/latency-annotated and carrying each instruction's
 *        current CuControlCode, in program order.
 * @param arch Kernel's architecture (unused for now: the 6-physical-slot scoreboard model is not
 *        expected to differ across this tool's Turing/Ampere target range -- see
 *        c_ScoreboardSlotCount).
 * @return CheckStatus::Verified with an empty ControlCodeCheckResult::violations if every hazard
 *         edge is correctly closed; CheckStatus::Violated with the offending edges listed
 *         otherwise. Never CheckStatus::NotImplemented -- that status remains only for
 *         correctControlCodes() (Reports/tasks.md Phase 4, not yet implemented).
 **/
inline ControlCodeCheckResult verifyControlCodes(const std::vector<DecodedInstruction>& decoded, const CuSMVersion& arch) {
    (void)arch;

    const HazardGraph graph = buildHazardGraph(decoded);
    std::vector<HazardViolation> violations = simulateAndVerify(decoded, graph);

    if (violations.empty()) {
        std::ostringstream oss;
        oss << "All hazards across " << decoded.size() << " instructions are correctly closed by their current control codes.";
        return ControlCodeCheckResult{CheckStatus::Verified, oss.str(), {}};
    }

    std::ostringstream oss;
    oss << violations.size() << " hazard edge(s) are not correctly closed by the current control codes.";
    return ControlCodeCheckResult{CheckStatus::Violated, oss.str(), std::move(violations)};
}

/**
 * @brief Which of a VARIABLE-latency producer's accesses (BarrierType::WRITE or
 *        BarrierType::READ) a hazard edge is about, mirroring simulateAndVerify()'s own
 *        "relevantAccess" rule exactly: RAW/WAW edges exist because the producer *wrote* the
 *        register, WAR edges exist because it *read* it.
 * @param type The hazard edge's RAW/WAW/WAR classification.
 * @return BarrierType::READ for a WAR edge, BarrierType::WRITE otherwise.
 **/
inline BarrierType relevantAccessOf(HazardType type) {
    return (type == HazardType::WAR) ? BarrierType::READ : BarrierType::WRITE;
}

/**
 * @brief Checks whether a producer's curated latency classification can protect a hazard edge
 *        whose relevantAccessOf() is as given -- the same "does this producer's barrier apply to
 *        this specific edge" test simulateAndVerify() applies when checking *existing* control
 *        codes (see its own doc: a VARIABLE producer's barrier only protects the specific access
 *        LatencyClassEntry::barrier names, its *other* access -- if it has one -- falls through to
 *        the FIXED-style adjacency rule), reused here (Phase 4) unchanged to decide which edges
 *        correction should close with a scoreboard wait versus a stall count.
 * @param producerLatency The producer instruction's curated LatencyClassEntry.
 * @param relevantAccess Which access the edge is about (relevantAccessOf(edge.type)).
 * @return True if producerLatency is LatencyKind::VARIABLE and its barrier matches relevantAccess.
 **/
inline bool isBarrierEligible(const LatencyClassEntry& producerLatency, BarrierType relevantAccess) {
    return producerLatency.kind == LatencyKind::VARIABLE && producerLatency.barrier.has_value() &&
           *producerLatency.barrier == relevantAccess;
}

/// One VARIABLE-latency producer's scoreboard-slot occupancy interval, for barrier-id
/// interval-coloring (see assignBarrierSlots()): the physical slot claimed when producerIndex
/// issues must stay reserved for it until firstConsumerIndex, the *earliest* instruction with a
/// barrier-eligible (isBarrierEligible()) hazard edge from producerIndex in this direction --
/// every *later* consumer of the same producer/direction is already covered for free once that
/// instruction's own wait retires producerIndex, since simulateAndVerify()'s per-producer
/// `retired` flag is permanent once set. So the slot is free for reuse starting at
/// firstConsumerIndex, not at producerIndex's *last* consumer -- a tighter (and still fully
/// correct) interval than "open until the last use" would give, which is what makes self-waiting
/// at only the earliest consumer (rather than every consumer) both sufficient and slot-efficient.
/// Two BarrierIntervals only need distinct slots if they actually overlap (see
/// assignBarrierSlots()); touching endpoints (one interval's firstConsumerIndex equal to
/// another's producerIndex) are fine, since simulateAndVerify() retires a slot's occupants (step
/// 1) before checking/claiming anything new on it (steps 2-3) within the same instruction.
struct BarrierInterval {
    std::size_t producerIndex;
    std::size_t firstConsumerIndex;
    BarrierType direction;
};

/**
 * @brief Finds every VARIABLE-latency producer in instrs that actually needs a scoreboard barrier
 *        and computes its resulting BarrierInterval (Reports/tasks.md Phase 4's "each barrier's
 *        lifetime... is an interval").
 *
 *        isBarrierEligible() alone is a per-*opcode* classification -- it says a producer's
 *        access *could* be barrier-protected, not that this specific instance actually needs a
 *        reserved slot. A real, working, ptxas-compiled schedule can (and does -- confirmed on a
 *        real GEMM kernel, Reports/correct-cc-overconservative-barrier-intervals.md) leave a
 *        VARIABLE-latency producer's barrier field completely unset when its first consumer
 *        already sits far enough away in program order, exactly the same "natural instruction
 *        spacing is close enough" judgment call this codebase already trusts ptxas to have made
 *        for FIXED-latency producers (simulateAndVerify()'s own doc: "it only catches the
 *        unconditionally-wrong case ... and treats every other ... edge as satisfied"). So a
 *        producer's *nearest* barrier-eligible consumer only earns it a real interval (and
 *        therefore a reserved physical slot) when that pair is genuinely adjacent with zero stall
 *        margin between them -- the one case this codebase's existing conservative model can't
 *        otherwise discharge. Every producer whose nearest consumer clears that bar by even one
 *        instruction is treated as already safely closed by natural spacing, the same way a
 *        FIXED-latency producer with that same gap already would be, and gets no interval at all.
 * @param instrs Kernel's decoded instructions, in program order.
 * @param graph Hazard graph for instrs, as returned by buildHazardGraph(instrs).
 * @return One BarrierInterval per producer that genuinely needs a reserved slot, ordered by
 *         producerIndex ascending -- required by assignBarrierSlots()'s greedy interval-coloring,
 *         which is only optimal (uses exactly the maximum simultaneous overlap) when intervals are
 *         processed in increasing start order.
 **/
inline std::vector<BarrierInterval> collectBarrierIntervals(const std::vector<DecodedInstruction>& instrs, const HazardGraph& graph) {
    std::map<std::size_t, BarrierInterval> needs;

    const auto [ei, eend] = boost::edges(graph);
    for (auto e = ei; e != eend; ++e) {
        const std::size_t producer = boost::source(*e, graph);
        const std::size_t consumer = boost::target(*e, graph);
        const BarrierType relevantAccess = relevantAccessOf(graph[*e].type);

        if (!isBarrierEligible(instrs[producer].latency, relevantAccess)) {
            continue;
        }

        const auto [it, inserted] = needs.try_emplace(producer, BarrierInterval{producer, consumer, relevantAccess});
        if (!inserted) {
            it->second.firstConsumerIndex = std::min(it->second.firstConsumerIndex, consumer);
        }
    }

    std::vector<BarrierInterval> result;
    result.reserve(needs.size());
    for (const auto& [producer, interval] : needs) {
        // Mirror simulateAndVerify()'s own FIXED-latency fallback test: only an immediately-
        // adjacent, zero-stall pair is unconditionally wrong. Anything else already has enough
        // natural spacing to be considered safe, so it needs no reserved slot at all.
        const bool adjacent = (interval.firstConsumerIndex == producer + 1);
        const bool zeroStall = instrs[producer].ctrlCode.getStallCount() == 0;
        if (!(adjacent && zeroStall)) {
            continue;
        }
        result.push_back(interval);
    }
    return result;
}

/// Result of assignBarrierSlots(): every needed producer's chosen physical scoreboard slot id.
struct BarrierAssignment {
    /// Maps BarrierInterval::producerIndex to its assigned slot id, in [0, slotsUsed).
    std::map<std::size_t, int> slotOf;
    /// Number of distinct slot ids actually used (<= c_ScoreboardSlotCount).
    int slotsUsed = 0;
};

/**
 * @brief Assigns each BarrierInterval a physical scoreboard slot id (Reports/tasks.md Phase 4's
 *        "barrier-id reassignment... as an interval-coloring problem"): builds an undirected
 *        interference graph over intervals (an edge between two iff they actually overlap -- see
 *        BarrierInterval's own doc for why touching endpoints don't count) with Boost.Graph, then
 *        colors it with boost::sequential_vertex_coloring, processing intervals in increasing
 *        start (producerIndex) order. Greedy coloring in increasing-left-endpoint order is a
 *        classical *exact* (not merely heuristic) algorithm specifically for interval graphs --
 *        it always uses exactly the maximum simultaneous overlap ("clique number") of colors, no
 *        more -- which is what lets this simple approach double as the "is 6 slots enough"
 *        feasibility check the report calls for, not just a best-effort packing.
 * @param intervals Barrier-needing producers, as returned by collectBarrierIntervals() (must be
 *        producerIndex-ascending, which that function already guarantees).
 * @return A BarrierAssignment mapping every interval's producerIndex to a slot id in
 *         [0, c_ScoreboardSlotCount), or std::nullopt if the interference graph's chromatic number
 *         exceeds c_ScoreboardSlotCount -- i.e. more than 6 of these producers are ever
 *         simultaneously in flight for this instruction order, which cannot be repaired without
 *         inserting an instruction to close one of them earlier (forbidden by the shuffler's own
 *         "never insert/remove instructions" constraint). This is the "reorder is provably
 *         unrepairable in place" case (Reports/tasks.md Phase 4 / report section 3).
 **/
inline std::optional<BarrierAssignment> assignBarrierSlots(const std::vector<BarrierInterval>& intervals) {
    using IntervalGraph = boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS>;

    IntervalGraph graph(intervals.size());
    for (std::size_t i = 0; i < intervals.size(); ++i) {
        for (std::size_t j = i + 1; j < intervals.size(); ++j) {
            const std::size_t maxStart = std::max(intervals[i].producerIndex, intervals[j].producerIndex);
            const std::size_t minEnd = std::min(intervals[i].firstConsumerIndex, intervals[j].firstConsumerIndex);
            if (maxStart < minEnd) {
                boost::add_edge(i, j, graph);
            }
        }
    }

    std::vector<int> colorOf(intervals.size());
    const auto colorMap = boost::make_iterator_property_map(colorOf.begin(), boost::get(boost::vertex_index, graph));
    const std::size_t numColors = boost::sequential_vertex_coloring(graph, colorMap);
    if (numColors > static_cast<std::size_t>(c_ScoreboardSlotCount)) {
        return std::nullopt;
    }

    BarrierAssignment assignment;
    assignment.slotsUsed = static_cast<int>(numColors);
    for (std::size_t i = 0; i < intervals.size(); ++i) {
        assignment.slotOf.emplace(intervals[i].producerIndex, colorOf[i]);
    }
    return assignment;
}

/// Raw control-code stall field width (CuControlCode::splitCode()'s 4-bit `0x0000f` mask,
/// Reports/tasks.md gotcha 5) -- the largest stall count a corrected instruction can encode.
inline constexpr std::uint32_t c_MaxStallCount = 15;

/**
 * @brief Runs real hazard-based control-code correction (Reports/tasks.md Phase 4): a full
 *        recompute of every instruction's waitbar/readbar/writebar/stall fields from scratch
 *        against the kernel's *current* instruction order -- correction never reorders, inserts,
 *        or removes instructions, only ever rewrites kcc.ctrlCodeList in place -- built directly
 *        on Phase 3's hazard-graph/scoreboard-simulator model:
 *          - Every barrier-eligible hazard edge (isBarrierEligible(), the same rule
 *            simulateAndVerify() itself uses to judge *existing* codes) is closed by assigning its
 *            producer a scoreboard slot (assignBarrierSlots(), the interval-coloring allocator
 *            over the 6 physical slots) and setting a wait bit for that slot on the producer's
 *            *earliest* barrier-eligible consumer only -- sufficient for every later consumer too,
 *            since simulateAndVerify()'s per-producer `retired` flag is permanent once any wait
 *            closes it (see BarrierInterval's doc).
 *          - Every other hazard edge (a FIXED-latency producer, or a VARIABLE producer's *other*
 *            access -- see simulateAndVerify()'s own doc) is closed the only way this codebase's
 *            model can: guaranteeing the producer does not immediately precede its consumer with a
 *            zero stall count, by raising the producer's stall count to at least 1 whenever that
 *            specific case would otherwise arise. This never *lowers* an existing stall count -- a
 *            corrected instruction's stall is max(original, required) -- so an already-valid
 *            kernel's timing margins are left alone unless a real violation needs more.
 *          - Every instruction's existing yield flag is preserved unchanged (CuControlCode::
 *            isYield()'s own doc: yield is orthogonal to scoreboard hazard correctness, so
 *            correction has no basis to touch it).
 *        If assignBarrierSlots() reports no valid <=6-slot assignment exists, this is the
 *        "reorder is provably unrepairable in place" case (Reports/tasks.md Phase 4 / report
 *        section 3): kcc.ctrlCodeList is left completely unchanged and CheckStatus::Unrepairable
 *        is returned, rather than emitting a still-broken best-effort result. Otherwise, before
 *        committing anything, the recomputed codes are re-verified from scratch against the same
 *        hazard graph (simulateAndVerify()) as a self-check -- this should always report zero
 *        residual violations by construction, but checking rather than assuming catches a bug in
 *        this function's own model rather than silently shipping a still-broken control code (see
 *        ControlCodeCheckResult::violations' doc for this rare failure mode).
 * @param kcc Kernel's control-code/instruction-code lists; ctrlCodeList is fully rewritten in
 *        place on success (CheckStatus::Corrected) and left completely unchanged otherwise
 *        (CheckStatus::Unrepairable) -- insCodeList and every list's size are never touched,
 *        preserving the "never insert/remove instructions" invariant correct-cc.cpp's own
 *        re-merge-size-check after this call depends on.
 * @param decoded kcc's instructions, already decoded (decodeInstructions()/
 *        decodeInstructionsFromSass()) against kcc's *current* ctrlCodeList -- role/latency-
 *        annotated and index-aligned 1:1 with kcc.ctrlCodeList/insCodeList.
 * @param arch Kernel's architecture (unused for now, see verifyControlCodes()'s identical note).
 * @return CheckStatus::Corrected (with kcc.ctrlCodeList rewritten) if every hazard this model can
 *         detect is now closed; CheckStatus::Unrepairable (with kcc.ctrlCodeList untouched) if no
 *         valid <=6-scoreboard-slot assignment exists for this instruction order, or (in the rare
 *         internal-self-check-failure case) if the recomputed codes still don't fully verify.
 *         Never CheckStatus::NotImplemented/Verified/Violated -- those remain verifyControlCodes()'s.
 **/
inline ControlCodeCheckResult correctControlCodes(KernelControlCodes& kcc, const std::vector<DecodedInstruction>& decoded,
                                                   const CuSMVersion& arch) {
    (void)arch;

    const HazardGraph graph = buildHazardGraph(decoded);

    const std::vector<BarrierInterval> intervals = collectBarrierIntervals(decoded, graph);
    const std::optional<BarrierAssignment> assignment = assignBarrierSlots(intervals);
    if (!assignment) {
        std::ostringstream oss;
        oss << "Cannot repair in place: " << intervals.size()
            << " VARIABLE-latency producer(s) need more scoreboard slots than the " << c_ScoreboardSlotCount
            << " physical slots available for this instruction order; control codes were left unchanged.";
        return ControlCodeCheckResult{CheckStatus::Unrepairable, oss.str(), {}};
    }

    // Every barrier-eligible edge is closed by a wait on its producer's assigned slot, placed on
    // that producer's earliest barrier-eligible consumer (BarrierInterval's own doc).
    std::vector<std::uint32_t> waitMask(decoded.size(), 0);
    std::vector<int> writeBarOf(decoded.size(), -1);
    std::vector<int> readBarOf(decoded.size(), -1);
    for (const BarrierInterval& interval : intervals) {
        const int slot = assignment->slotOf.at(interval.producerIndex);
        waitMask[interval.firstConsumerIndex] |= (1u << slot);
        (interval.direction == BarrierType::WRITE ? writeBarOf : readBarOf)[interval.producerIndex] = slot;
    }

    // Every other edge falls back to the FIXED-style conservative rule simulateAndVerify() itself
    // checks against: only an immediately-adjacent, zero-stall pair is unconditionally wrong, so
    // that is the only case correction needs to fix here.
    std::vector<std::uint32_t> requiredStall(decoded.size(), 0);
    {
        const auto [ei, eend] = boost::edges(graph);
        for (auto e = ei; e != eend; ++e) {
            const std::size_t producer = boost::source(*e, graph);
            const std::size_t consumer = boost::target(*e, graph);
            const BarrierType relevantAccess = relevantAccessOf(graph[*e].type);
            if (isBarrierEligible(decoded[producer].latency, relevantAccess)) {
                continue;
            }
            if (consumer == producer + 1) {
                requiredStall[producer] = std::max(requiredStall[producer], std::uint32_t{1});
            }
        }
    }

    std::vector<std::uint32_t> newCodes(decoded.size());
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        const std::uint32_t waitbar = waitMask[i];
        const std::uint32_t readbar = (readBarOf[i] >= 0) ? static_cast<std::uint32_t>(readBarOf[i]) : 7u;
        const std::uint32_t writebar = (writeBarOf[i] >= 0) ? static_cast<std::uint32_t>(writeBarOf[i]) : 7u;
        const std::uint32_t yieldFlag = decoded[i].ctrlCode.isYield() ? 0u : 1u;
        const std::uint32_t stall = std::max(decoded[i].ctrlCode.getStallCount(), requiredStall[i]);
        if (stall > c_MaxStallCount) {
            // Unreachable today -- requiredStall is always 0 or 1 here, and an already-decoded
            // stall count can never exceed 15 either (CuControlCode::splitCode()'s own 4-bit
            // mask) -- but kept as an explicit reject rather than silently truncating/overflowing,
            // in case this model ever grows a real per-opcode numeric latency table (see
            // simulateAndVerify()'s own doc on its current FIXED-latency limitation).
            std::ostringstream oss;
            oss << "Cannot repair in place: instruction " << i << " would need a stall count of " << stall
                << ", exceeding the 4-bit (0-" << c_MaxStallCount << ") field width; control codes were left unchanged.";
            return ControlCodeCheckResult{CheckStatus::Unrepairable, oss.str(), {}};
        }
        newCodes[i] = CuControlCode::mergeCode(waitbar, readbar, writebar, yieldFlag, stall);
    }

    std::vector<DecodedInstruction> corrected = decoded;
    for (std::size_t i = 0; i < corrected.size(); ++i) {
        corrected[i].ctrlCode = CuControlCode(newCodes[i]);
    }
    const std::vector<HazardViolation> residual = simulateAndVerify(corrected, graph);
    if (!residual.empty()) {
        std::ostringstream oss;
        oss << "Internal error: recomputed control codes still leave " << residual.size()
            << " hazard edge(s) unresolved; control codes were left unchanged.";
        return ControlCodeCheckResult{CheckStatus::Unrepairable, oss.str(), residual};
    }

    kcc.ctrlCodeList = std::move(newCodes);

    std::ostringstream oss;
    oss << "Recomputed control codes for " << decoded.size() << " instructions (" << assignment->slotsUsed << "/"
        << c_ScoreboardSlotCount << " scoreboard slots used).";
    return ControlCodeCheckResult{CheckStatus::Corrected, oss.str(), {}};
}

} // namespace CuAsm::Tools
