#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <elfio/elfio.hpp>

#include "../../bin/ccCommon.hpp"

// Clean C++ entry point for the `correct-cc` CLI tool (bin/correct-cc.cpp): loading a cubin,
// decoding every kernel's instructions, running real hazard-based correction (ccCommon.hpp's
// correctControlCodes(), Reports/tasks.md Phase 4) against each one, and -- only if every kernel
// was successfully repaired -- patching the corrected control codes back into a copy of the
// original cubin bytes and writing it out. Section sizes never change: correction only ever
// rewrites existing control-code fields, never adds/removes instructions.
//
// Unlike the CLI, this throws on any load/decode/re-merge-size-mismatch failure instead of
// logging and returning false, never touches CuAsmLogger/std::cout, and always overwrites an
// existing output path (see CuAsmTools/Cuasm.hpp's header doc for why the CLI's backup-existing-
// output-file behavior isn't replicated here). It does NOT throw when a kernel is
// CheckStatus::Unrepairable -- that is an expected, documented outcome (see
// correctControlCodes()'s own doc), reported via CubinCorrectionReport::anyUnrepairable/wrote
// exactly as bin/correct-cc.cpp's own exit code communicates it, rather than forcing every caller
// to catch an exception for a routine "this reorder cannot be repaired in place" result.
//
// SCOPE: Turing (sm_75) and newer only, same as every other tool in this header group -- see
// ccCommon.hpp's c_MinSupportedSMVersion.

namespace CuAsm::Tools {

/// Correction outcome for one kernel.
struct KernelCorrectionReport {
    std::string kernelName;
    /// The correction outcome: CheckStatus::Corrected or CheckStatus::Unrepairable (never
    /// Verified/Violated/NotImplemented -- those remain verifyControlCodes()-only).
    ControlCodeCheckResult result;
};

/// Correction outcome for an entire cubin.
struct CubinCorrectionReport {
    CuSMVersion arch;
    /// One entry per kernel in the cubin, in section order.
    std::vector<KernelCorrectionReport> kernels;
    /// True if any kernel's control codes could not be repaired in place (kernels.result.status ==
    /// CheckStatus::Unrepairable for at least one entry).
    bool anyUnrepairable = false;
    /// True if the corrected cubin was actually written to outPath -- false when anyUnrepairable,
    /// matching bin/correct-cc.cpp's "don't silently write a cubin that still contains a real
    /// hazard" contract.
    bool wrote = false;
};

/**
 * @brief Loads a cubin, decodes every kernel's instructions, runs real hazard-based control-code
 *        correction on each one, and -- only if every kernel was successfully repaired -- writes
 *        the result to a new cubin file by patching each kernel's ".text.<kernel>" bytes in place,
 *        mirroring bin/correct-cc.cpp's own correctCC() local function minus its logging.
 * @param inPath Input cubin path.
 * @param outPath Output cubin path; only actually written if every kernel was repairable (see
 *        CubinCorrectionReport::wrote).
 * @return The correction report for every kernel in the cubin.
 * @throws std::runtime_error if the cubin fails to load, targets an unsupported SM version (older
 *         than Turing/sm_75), contains no ".text.<kernel>" sections, or a corrected kernel
 *         re-merges to a different section size than it started with (an internal modeling bug,
 *         not an expected outcome -- correction never adds/removes instructions).
 * @throws CuAsm::CalledProcessError, std::exception See decodeInstructions() for decode failures
 *         (missing cuobjdump, an uncurated opcode, ...).
 **/
inline CubinCorrectionReport correctCubinControlCodes(const std::string& inPath, const std::string& outPath) {
    ELFIO::elfio ef;
    if (!ef.load(inPath)) {
        throw std::runtime_error("correctCubinControlCodes: failed to load ELF/cubin \"" + inPath + "\"");
    }

    const auto arch = detectArch(ef);
    if (!arch) {
        throw std::runtime_error("correctCubinControlCodes: cubin \"" + inPath +
                                  "\" targets an unsupported SM version -- requires Turing (sm_75) or newer");
    }

    std::vector<KernelControlCodes> kernels = loadControlCodes(ef, *arch);
    if (kernels.empty()) {
        throw std::runtime_error("correctCubinControlCodes: no \".text.<kernel>\" sections found in \"" + inPath + "\"");
    }

    const std::map<std::string, std::vector<DecodedInstruction>> decodedByKernel = decodeInstructions(inPath, kernels, *arch);

    std::ifstream in(inPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string fileContents = ss.str();
    std::vector<char> outBytes(fileContents.begin(), fileContents.end());

    CubinCorrectionReport report{*arch};

    for (auto& kcc : kernels) {
        ControlCodeCheckResult result = correctControlCodes(kcc, decodedByKernel.at(kcc.kernelName), *arch);
        report.anyUnrepairable |= (result.status == CheckStatus::Unrepairable);

        const std::vector<std::byte> merged = arch->mergeCtrlCodes(kcc.insCodeList, kcc.ctrlCodeList);
        if (merged.size() != kcc.sectionSize) {
            throw std::runtime_error("correctCubinControlCodes: re-merged control codes for kernel \"" + kcc.kernelName +
                                      "\" changed section size (" + std::to_string(kcc.sectionSize) + " -> " +
                                      std::to_string(merged.size()) + " bytes)");
        }
        std::memcpy(outBytes.data() + kcc.sectionOffset, merged.data(), merged.size());

        report.kernels.push_back(KernelCorrectionReport{kcc.kernelName, std::move(result)});
    }

    if (!report.anyUnrepairable) {
        std::ofstream out(outPath, std::ios::binary);
        out.write(outBytes.data(), static_cast<std::streamsize>(outBytes.size()));
        report.wrote = true;
    }

    return report;
}

} // namespace CuAsm::Tools
