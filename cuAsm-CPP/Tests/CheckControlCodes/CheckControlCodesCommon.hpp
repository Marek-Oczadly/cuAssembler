#pragma once

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <elfio/elfio.hpp>

#include "../../CuAsm/common.hpp"
#include "../../bin/ccCommon.hpp"

// Reports/tasks.md Phase 5's "fixture-based tests against real cuobjdump-decoded kernels ... to
// validate role/latency table content" item. Reuses TestData/CheckDisasm/<Kernel>/<Kernel>.cu --
// the same real-world .cu fixtures CheckDisasm already round-trips through the assembler/
// disassembler -- instead of introducing a second fixture set, since these already exercise a wide
// spread of real opcodes across many opcode families (atomics, textures, tensor cores, control
// flow, ...), which is exactly the breadth Phase 0/1's curated OperandRoleTable/LatencyClassTable
// need real-compiler evidence against. Where CheckDisasmCommon.hpp checks that reassembly
// reproduces nvcc's exact bytes, this checks that ccCommon.hpp's decode + hazard-verification
// pipeline (Reports/tasks.md Phases 2/3) accepts every opcode nvcc/ptxas actually emitted and
// agrees that ptxas's own real control codes correctly close every hazard -- a wrong or missing
// role/latency table entry surfaces here as a hard decode error (std::out_of_range from an
// uncurated InsKey) or a real CheckStatus::Violated (the curated data disagrees with how a real,
// working compiler scheduled this exact instruction), neither of which any hand-built synthetic
// DecodedInstruction sequence (test_ccHazard.cpp/test_ccCorrect.cpp) can catch.

namespace CuAsm::Test {

namespace fs = std::filesystem;

/**
 * @brief Removes a file if it exists, ignoring any error.
 * @param path Path of the file to remove.
 **/
inline void removeQuietCC(const std::string& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

/**
 * @brief Extracts the numeric SM version from an architecture string, e.g. "sm_75" -> 75.
 * @param arch Target SM architecture string, e.g. "sm_75".
 * @return The architecture's numeric SM version.
 **/
inline int smArchNumberCC(const std::string& arch) {
    const std::size_t underscore = arch.find('_');
    return std::stoi(arch.substr(underscore + 1));
}

/**
 * @brief Runs the CheckControlCodes round trip for one (kernel, architecture) pair: compiles the
 *        kernel's .cu fixture (shared with CheckDisasm, under TestData/CheckDisasm/<kernel>) with
 *        nvcc, decodes every resulting kernel function's real ptxas-scheduled control codes via
 *        ccCommon.hpp's decodeInstructions() (Reports/tasks.md Phase 2), and runs hazard-based
 *        verifyControlCodes() (Phase 3) against each one. Any intermediate cubin left over from a
 *        previous run is cleared before this run starts.
 * @param kernelName Name of the kernel/subdirectory under TestData/CheckDisasm; also the base
 *        name of the .cu fixture.
 * @param arch Target SM architecture, e.g. "sm_75".
 * @param minArchSM Minimum numeric SM version (e.g. 80) the kernel's instructions require,
 *        mirroring CheckDisasmCommon.hpp's runCheckDisasm() parameter of the same name -- if
 *        arch's numeric SM version is lower, the round trip is skipped and this automatically
 *        passes. 0 (the default) means no minimum.
 * @return true if every kernel function the fixture produces decodes without error and verifies
 *         with CheckStatus::Verified, or if the check was skipped because arch is below
 *         minArchSM; false on any compile/load/decode error or a reported hazard violation.
 **/
inline bool runCheckControlCodes(const std::string& kernelName, const std::string& arch, int minArchSM = 0) {
    if (minArchSM > 0 && smArchNumberCC(arch) < minArchSM) {
        std::cout << "[SKIP] CheckControlCodes " << kernelName << " (" << arch << "): requires sm_" << minArchSM
                  << " or newer\n";
        return true;
    }

    const std::string dir = std::string(CUASM_TESTDATA_DIR) + "/CheckDisasm/" + kernelName;
    const std::string cuFile = dir + "/" + kernelName + ".cu";
    const std::string cubinFile = dir + "/" + kernelName + "." + arch + ".cc.cubin";

    removeQuietCC(cubinFile);

    bool passed = false;
    std::string failureReason;

    try {
        CuAsm::checkOutput({"nvcc", cuFile, "-arch=" + arch, "-cubin", "-o", cubinFile}, true);

        ELFIO::elfio ef;
        if (!ef.load(cubinFile)) {
            throw std::runtime_error("failed to load cubin \"" + cubinFile + "\" via ELFIO");
        }

        const auto sm = CuAsm::Tools::detectArch(ef);
        if (!sm) {
            throw std::runtime_error("cubin \"" + cubinFile + "\" has an unsupported/undetected SM version");
        }

        const std::vector<CuAsm::Tools::KernelControlCodes> kernels = CuAsm::Tools::loadControlCodes(ef, *sm);
        if (kernels.empty()) {
            throw std::runtime_error("no \".text.<kernel>\" sections found in \"" + cubinFile + "\"");
        }

        const std::map<std::string, std::vector<CuAsm::Tools::DecodedInstruction>> decodedByKernel =
            CuAsm::Tools::decodeInstructions(cubinFile, kernels, *sm);

        std::size_t totalInstrs = 0;
        for (const CuAsm::Tools::KernelControlCodes& kcc : kernels) {
            const CuAsm::Tools::ControlCodeCheckResult result =
                CuAsm::Tools::verifyControlCodes(decodedByKernel.at(kcc.kernelName), *sm);
            totalInstrs += kcc.ctrlCodeList.size();
            if (result.status != CuAsm::Tools::CheckStatus::Verified) {
                std::ostringstream oss;
                oss << "kernel \"" << kcc.kernelName << "\": " << result.message;
                for (const CuAsm::Tools::HazardViolation& v : result.violations) {
                    oss << "\n    instruction " << v.consumerIndex << " has an unresolved " << CuAsm::Tools::toString(v.type)
                        << " hazard from instruction " << v.producerIndex << " on " << CuAsm::Tools::toString(v.regSpace)
                        << v.regNumber << ": " << v.reason;
                }
                throw std::runtime_error(oss.str());
            }
        }

        passed = true;
        std::cout << "[PASS] CheckControlCodes " << kernelName << " (" << arch << "): " << kernels.size()
                  << " kernel(s), " << totalInstrs << " instruction(s) all verified clean\n";
    } catch (const CuAsm::CalledProcessError& e) {
        failureReason = std::string(e.what()) + ": " + e.output();
    } catch (const std::exception& e) {
        failureReason = e.what();
    }

    if (!passed) {
        std::cerr << "[FAIL] CheckControlCodes " << kernelName << " (" << arch << "): " << failureReason << "\n";
    }

    return passed;
}

} // namespace CuAsm::Test
