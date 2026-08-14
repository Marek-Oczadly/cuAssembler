#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <elfio/elfio.hpp>

#include "../CuAsm/CuSMVersion.hpp"
#include "../CuAsm/utils/BigNum.hpp"

// Shared helpers for the verify-cc/correct-cc control-code tools: loading a cubin's per-kernel
// control-code/instruction-code lists, and the verification/correction entry points they both
// call. verifyControlCodes()/correctControlCodes() below are placeholders -- see
// Reports/control-codes-validation.md for the scope real hazard-based verification/correction
// needs (an operand read/write role table, a latency/pipe table, a dependency graph, and a
// scoreboard-state simulator, none of which exist anywhere in this codebase yet).

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

/**
 * @brief Detects a cubin's target architecture from its ELF header flags, mirroring
 *        CubinFile::loadCubin's own "smVersion = e_flags & 0xff" derivation.
 * @param ef Already-loaded ELF reader for the cubin.
 * @return The detected CuSMVersion, or std::nullopt if the encoded SM version isn't one
 *         CuSMVersion accepts (see CuSMVersion::validSMVersions()).
 **/
inline std::optional<CuSMVersion> detectArch(const ELFIO::elfio& ef) {
    const auto smVersion = static_cast<int>(ef.get_flags() & 0xff);
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
