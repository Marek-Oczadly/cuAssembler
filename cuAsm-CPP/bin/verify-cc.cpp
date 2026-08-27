// Verify the scoreboard control codes of a cubin's instructions.
//
// This loads a cubin, detects its architecture, decodes every kernel's instructions (control
// codes plus role/latency-annotated operand data, via ccCommon.hpp's decodeInstructions()), and
// runs real hazard-based verification (ccCommon.hpp's verifyControlCodes(), Reports/tasks.md
// Phase 3): every RAW/WAW/WAR dependency implied by the decoded instructions is checked against a
// 6-slot scoreboard simulation of the kernel's *current* control codes. See
// verifyControlCodes()/simulateAndVerify()'s own docs for the one known limitation this carries
// (FIXED-latency hazards are checked against a conservative lower bound, not an exact cycle
// count, since no per-opcode numeric latency table exists in this codebase). Correction
// (repairing a violation) is not implemented yet -- see correct-cc / Reports/tasks.md Phase 4.
//
// SCOPE: Turing (sm_75) and newer only. Older architectures are rejected up front (see
// ccCommon.hpp's c_MinSupportedSMVersion) since the operand-role/latency data this tool needs
// is best documented from Turing onward.
//
// Examples:
//     verify-cc a.cubin
//         decode and list every kernel's control codes
//
//     verify-cc a.cubin -k myKernel
//         restrict output to one kernel
//
//     verify-cc a.cubin -v
//         verbose logging

#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <elfio/elfio.hpp>

#include "../CuAsm/CuAsmLogger.hpp"
#include "../CuAsm/CuControlCode.hpp"
#include "../CuAsm/common.hpp"
#include "ccCommon.hpp"
#include "cliCommon.hpp"

namespace {

/** @brief Formats a value (address or BigInt instruction code) as a "0x..." hex string. */
template <typename T>
std::string toHexString(const T& v) {
    std::ostringstream oss;
    oss << std::hex << std::showbase << v;
    return oss.str();
}

/**
 * @brief Loads a cubin, decodes each kernel's instructions, and runs real hazard-based
 *        verification against each one (ccCommon.hpp's verifyControlCodes(), Reports/tasks.md
 *        Phase 3), printing a per-instruction listing plus the outcome.
 * @param fin Input cubin path.
 * @param kernelFilter If non-empty, restrict output to the kernel with this exact name.
 * @return True if the cubin loaded, decoded, and every kernel was checked (NOT that verification
 *         passed -- a kernel can still print "FAILED", see the per-kernel status line); false on
 *         load failure, an unsupported SM version, a decode failure (missing cuobjdump, an
 *         uncurated opcode, ...), or an unmatched --kernel filter.
 **/
bool verifyCC(const std::string& fin, const std::string& kernelFilter) {
    ELFIO::elfio ef;
    if (!ef.load(fin)) {
        CuAsm::CuAsmLogger::logError("Failed to load ELF/cubin \"" + fin + "\"!");
        return false;
    }

    const auto arch = CuAsm::Tools::detectArch(ef);
    if (!arch) {
        CuAsm::CuAsmLogger::logError("Cubin \"" + fin +
                                      "\" targets an unsupported SM version -- verify-cc requires Turing "
                                      "(sm_75) or newer!");
        return false;
    }
    CuAsm::CuAsmLogger::logProcedure("Detected " + arch->getVersionString() + ".");

    const std::vector<CuAsm::Tools::KernelControlCodes> kernels = CuAsm::Tools::loadControlCodes(ef, *arch);
    if (kernels.empty()) {
        CuAsm::CuAsmLogger::logError("No \".text.<kernel>\" sections found in \"" + fin + "\"!");
        return false;
    }

    std::map<std::string, std::vector<CuAsm::Tools::DecodedInstruction>> decodedByKernel;
    try {
        decodedByKernel = CuAsm::Tools::decodeInstructions(fin, kernels, *arch);
    } catch (const CuAsm::CalledProcessError& cpe) {
        CuAsm::CuAsmLogger::logError(std::string("Error when running cuobjdump: ") + cpe.output());
        return false;
    } catch (const std::exception& e) {
        CuAsm::CuAsmLogger::logError(std::string("Failed to decode instructions for hazard verification: ") + e.what());
        return false;
    }

    bool foundFilterMatch = kernelFilter.empty();
    for (const auto& kcc : kernels) {
        if (!kernelFilter.empty() && kcc.kernelName != kernelFilter) {
            continue;
        }
        foundFilterMatch = true;

        std::cout << "Kernel: " << kcc.kernelName << "  (" << kcc.ctrlCodeList.size() << " instructions, "
                   << arch->getVersionString() << ")\n";
        for (std::size_t i = 0; i < kcc.ctrlCodeList.size(); ++i) {
            const std::uint64_t addr = arch->getInsOffsetFromIndex(static_cast<int>(i));
            std::cout << "  [" << toHexString(addr) << "] " << CuAsm::CuControlCode::decode(kcc.ctrlCodeList[i])
                       << "  code=" << toHexString(kcc.insCodeList[i]) << "\n";
        }

        const CuAsm::Tools::ControlCodeCheckResult result =
            CuAsm::Tools::verifyControlCodes(decodedByKernel.at(kcc.kernelName), *arch);
        switch (result.status) {
        case CuAsm::Tools::CheckStatus::Verified:
            std::cout << "  Verification: PASSED -- " << result.message << "\n\n";
            break;
        case CuAsm::Tools::CheckStatus::Violated:
            std::cout << "  Verification: FAILED -- " << result.message << "\n";
            for (const auto& v : result.violations) {
                std::cout << "    instruction " << v.consumerIndex << " has an unresolved " << CuAsm::Tools::toString(v.type)
                           << " hazard from instruction " << v.producerIndex << " on " << CuAsm::Tools::toString(v.regSpace)
                           << v.regNumber << ": " << v.reason << "\n";
            }
            std::cout << "\n";
            break;
        case CuAsm::Tools::CheckStatus::NotImplemented:
            std::cout << "  Verification: NOT IMPLEMENTED -- " << result.message << "\n\n";
            break;
        }
    }

    if (!foundFilterMatch) {
        CuAsm::CuAsmLogger::logError("Kernel \"" + kernelFilter + "\" not found in \"" + fin + "\"!");
        return false;
    }

    return true;
}

} // namespace

/**
 * @brief Entry point for the verify-cc tool: parses arguments, sets up logging, and lists (and
 *        attempts to verify) a cubin's control codes.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success; -1 on any I/O, decode, or filter-match error; CLI11's own exit code on a
 *         bad command line (see CLI11_PARSE).
 **/
int main(int argc, char** argv) {
    CLI::App app{"Verify scoreboard control codes of a cubin's instructions "
                 "(RAW/WAW/WAR hazard verification -- see Reports/control-codes-validation.md)."};

    std::string infile;
    std::string kernelFilter;
    std::string logfile;
    bool verbose = false;
    bool quiet = false;

    app.add_option("infile", infile, "Input cubin file")->required();
    app.add_option("-k,--kernel", kernelFilter, "Restrict output to one kernel by name");
    CLI::Option* logOpt = app.add_option("-f,--logfile", logfile, "Log file");
    app.add_flag("-v,--verbose", verbose, "Verbose output");
    app.add_flag("-q,--quiet", quiet, "Quiet output");

    CLI11_PARSE(app, argc, argv);

    if (!checkInFileExistence(infile)) {
        return -1;
    }

    int stdoutLevel;
    int fileLevel;
    if (verbose) {
        stdoutLevel = 0;
        fileLevel = 0;
    } else if (quiet) {
        stdoutLevel = static_cast<int>(CuAsm::LogLevel::Error);
        fileLevel = 25;
    } else {
        stdoutLevel = static_cast<int>(CuAsm::LogLevel::Procedure);
        fileLevel = static_cast<int>(CuAsm::LogLevel::Info);
    }

    if (logOpt->count() > 0) {
        CuAsm::CuAsmLogger::initLogger(logfile, fileLevel, stdoutLevel);
    } else {
        CuAsm::CuAsmLogger::initLogger("", fileLevel, stdoutLevel, "cuasm", std::size_t(1) << 30, 3, false);
    }

    return verifyCC(infile, kernelFilter) ? 0 : -1;
}
