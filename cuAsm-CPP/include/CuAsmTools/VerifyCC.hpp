#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <elfio/elfio.hpp>

#include "../../bin/ccCommon.hpp"

// Clean C++ entry point for the `verify-cc` CLI tool (bin/verify-cc.cpp): loading a cubin,
// decoding every kernel's instructions, and running real hazard-based verification
// (ccCommon.hpp's verifyControlCodes(), Reports/tasks.md Phase 3) against each one. ccCommon.hpp's
// own functions (detectArch/loadControlCodes/decodeInstructions/verifyControlCodes) already are a
// clean, structured, exception-based API -- what bin/verify-cc.cpp's local verifyCC() function
// adds on top, and what this header actually contributes, is the "whole cubin file in, one
// verification report out" orchestration plus a return type that carries everything the CLI's own
// per-instruction listing printed, so a caller doesn't have to re-run the load/decode steps itself
// to reproduce it.
//
// SCOPE: Turing (sm_75) and newer only, same as every other tool in this header group -- see
// ccCommon.hpp's c_MinSupportedSMVersion.

namespace CuAsm::Tools {

/// Verification outcome for one kernel, carrying everything bin/verify-cc.cpp's own per-
/// instruction listing needs to reproduce it (kcc.ctrlCodeList/insCodeList, decoded), plus the
/// ControlCodeCheckResult verifyControlCodes() itself produced.
struct KernelVerificationReport {
    /// This kernel's control-code/instruction-code lists, as loaded by loadControlCodes().
    KernelControlCodes kcc;
    /// This kernel's instructions, fully decoded (role/latency-annotated) -- the same data
    /// verifyControlCodes() was run against.
    std::vector<DecodedInstruction> decoded;
    /// The hazard-verification outcome for this kernel.
    ControlCodeCheckResult result;
};

/// Verification outcome for an entire cubin: one KernelVerificationReport per matched kernel, in
/// section order, plus the detected architecture.
struct CubinVerificationReport {
    CuSMVersion arch;
    std::vector<KernelVerificationReport> kernels;
};

/**
 * @brief Loads a cubin, decodes each kernel's instructions, and runs real hazard-based
 *        verification against each one, mirroring bin/verify-cc.cpp's own verifyCC() local
 *        function -- minus its stdout printing, which callers can reproduce from the returned
 *        report (or skip entirely).
 * @param cubinPath Input cubin path.
 * @param kernelFilter If non-empty, restrict the report to the kernel with this exact name.
 * @return The verification report: one entry per matched kernel (all of them if kernelFilter is empty).
 * @throws std::runtime_error if the cubin fails to load, targets an unsupported SM version (older
 *         than Turing/sm_75), or contains no ".text.<kernel>" sections.
 * @throws std::invalid_argument if kernelFilter is non-empty and matches no kernel in the cubin.
 * @throws CuAsm::CalledProcessError, std::exception See decodeInstructions() for decode failures
 *         (missing cuobjdump, an uncurated opcode, ...).
 **/
inline CubinVerificationReport verifyCubinControlCodes(const std::string& cubinPath, const std::string& kernelFilter = "") {
    ELFIO::elfio ef;
    if (!ef.load(cubinPath)) {
        throw std::runtime_error("verifyCubinControlCodes: failed to load ELF/cubin \"" + cubinPath + "\"");
    }

    const auto arch = detectArch(ef);
    if (!arch) {
        throw std::runtime_error("verifyCubinControlCodes: cubin \"" + cubinPath +
                                  "\" targets an unsupported SM version -- requires Turing (sm_75) or newer");
    }

    std::vector<KernelControlCodes> kernels = loadControlCodes(ef, *arch);
    if (kernels.empty()) {
        throw std::runtime_error("verifyCubinControlCodes: no \".text.<kernel>\" sections found in \"" + cubinPath + "\"");
    }

    const std::map<std::string, std::vector<DecodedInstruction>> decodedByKernel = decodeInstructions(cubinPath, kernels, *arch);

    CubinVerificationReport report{*arch};

    bool foundFilterMatch = kernelFilter.empty();
    for (auto& kcc : kernels) {
        if (!kernelFilter.empty() && kcc.kernelName != kernelFilter) {
            continue;
        }
        foundFilterMatch = true;

        const std::vector<DecodedInstruction>& decoded = decodedByKernel.at(kcc.kernelName);
        KernelVerificationReport kr;
        kr.result = verifyControlCodes(decoded, *arch);
        kr.decoded = decoded;
        kr.kcc = std::move(kcc);
        report.kernels.push_back(std::move(kr));
    }

    if (!foundFilterMatch) {
        throw std::invalid_argument("verifyCubinControlCodes: kernel \"" + kernelFilter + "\" not found in \"" + cubinPath + "\"");
    }

    return report;
}

} // namespace CuAsm::Tools
