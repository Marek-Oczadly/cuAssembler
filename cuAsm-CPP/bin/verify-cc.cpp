// Verify the scoreboard control codes of a cubin's instructions.
//
// CURRENT STATUS: this is a thin CLI skeleton. It loads a cubin, detects its architecture,
// decodes every kernel's control codes via the existing CuSMVersion/CuControlCode machinery,
// and lists them -- but the actual hazard check (verifyControlCodes(), in ccCommon.hpp) is a
// placeholder that always reports "not implemented". Real verification needs an operand
// read/write role table, a latency/pipe table, and a scoreboard-state simulator that don't
// exist anywhere in this codebase yet; see Reports/control-codes-validation.md for the full
// scope. A clean run of this tool means "the control codes were decoded", not "they're correct".
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
#include <sstream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <elfio/elfio.hpp>

#include "../CuAsm/CuAsmLogger.hpp"
#include "../CuAsm/CuControlCode.hpp"
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
 * @brief Loads a cubin, decodes each kernel's control codes, and runs (currently stubbed)
 *        verification against each one, printing a per-instruction listing plus the outcome.
 * @param fin Input cubin path.
 * @param kernelFilter If non-empty, restrict output to the kernel with this exact name.
 * @return True if the cubin loaded and its control codes decoded successfully (NOT that
 *         verification passed -- see the printed "NOT IMPLEMENTED" status); false on load
 *         failure, an unsupported SM version, or an unmatched --kernel filter.
 **/
bool verifyCC(const std::string& fin, const std::string& kernelFilter) {
    ELFIO::elfio ef;
    if (!ef.load(fin)) {
        CuAsm::CuAsmLogger::logError("Failed to load ELF/cubin \"" + fin + "\"!");
        return false;
    }

    const auto arch = CuAsm::Tools::detectArch(ef);
    if (!arch) {
        CuAsm::CuAsmLogger::logError("Cubin \"" + fin + "\" targets an unsupported/unrecognized SM version!");
        return false;
    }
    CuAsm::CuAsmLogger::logProcedure("Detected " + arch->getVersionString() + ".");

    const std::vector<CuAsm::Tools::KernelControlCodes> kernels = CuAsm::Tools::loadControlCodes(ef, *arch);
    if (kernels.empty()) {
        CuAsm::CuAsmLogger::logError("No \".text.<kernel>\" sections found in \"" + fin + "\"!");
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

        const CuAsm::Tools::ControlCodeCheckResult result = CuAsm::Tools::verifyControlCodes(kcc, *arch);
        std::cout << "  Verification: NOT IMPLEMENTED -- " << result.message << "\n\n";
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
                 "(structural decode only for now -- see Reports/control-codes-validation.md)."};

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
