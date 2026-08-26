#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

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
// records (decodeInstructions(), Reports/tasks.md Phase 2), and the verification/correction entry
// points they both call. verifyControlCodes()/correctControlCodes() below are still placeholders
// -- see Reports/control-codes-validation.md for the scope real hazard-based verification/
// correction needs on top of decodeInstructions() (a dependency graph and a scoreboard-state
// simulator, Reports/tasks.md Phase 3, neither of which exist yet).

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

/// Outcome of a verification or correction pass.
enum class CheckStatus {
    NotImplemented,
};

/// Result of running verifyControlCodes()/correctControlCodes() against one kernel.
struct ControlCodeCheckResult {
    CheckStatus status = CheckStatus::NotImplemented;
    std::string message;
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
 * @brief Placeholder for hazard-based control-code verification (checking that every RAW/WAW/
 *        WAR dependency is still correctly guarded after a reorder). NOT YET IMPLEMENTED: doing
 *        this for real needs an operand read/write role table, a latency/pipe classification
 *        table, and a scoreboard-state simulator, none of which exist in this codebase yet --
 *        see Reports/control-codes-validation.md (sections 2 and 6, "Phase 0-2"). This stub
 *        makes no claim about hazard correctness; it only confirms the control codes decoded.
 * @param kcc Kernel's decoded control-code/instruction-code lists to check.
 * @param arch Kernel's architecture (unused for now; needed for scoreboard-slot capacity later).
 * @return Always CheckStatus::NotImplemented, with an explanatory message.
 **/
inline ControlCodeCheckResult verifyControlCodes(const KernelControlCodes& kcc, const CuSMVersion& arch) {
    (void)kcc;
    (void)arch;
    return ControlCodeCheckResult{
        CheckStatus::NotImplemented,
        "Hazard-based verification is not implemented yet (see Reports/control-codes-validation.md)."};
}

/**
 * @brief Placeholder for hazard-based control-code correction (barrier-id reassignment and
 *        stall-count recomputation after a reorder, without inserting/removing instructions).
 *        NOT YET IMPLEMENTED: currently a no-op that leaves kcc.ctrlCodeList unchanged -- see
 *        Reports/control-codes-validation.md (section 3, "Phase 3") for the barrier-slot
 *        allocation and reject/rollback logic a real implementation needs.
 * @param kcc Kernel's decoded control-code/instruction-code lists; ctrlCodeList is left as-is.
 * @param arch Kernel's architecture (unused for now).
 * @return Always CheckStatus::NotImplemented, with an explanatory message.
 **/
inline ControlCodeCheckResult correctControlCodes(KernelControlCodes& kcc, const CuSMVersion& arch) {
    (void)kcc;
    (void)arch;
    return ControlCodeCheckResult{
        CheckStatus::NotImplemented,
        "Hazard-based correction is not implemented yet (see Reports/control-codes-validation.md); "
        "control codes were left unchanged."};
}

} // namespace CuAsm::Tools
