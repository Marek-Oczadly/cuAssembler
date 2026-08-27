// Correct the scoreboard control codes of a cubin's instructions.
//
// This loads a cubin, decodes every kernel's instructions (control codes plus role/latency-
// annotated operand data, via ccCommon.hpp's decodeInstructions()), and runs real hazard-based
// correction (ccCommon.hpp's correctControlCodes(), Reports/tasks.md Phase 4): every RAW/WAW/WAR
// dependency implied by the decoded instructions is closed by recomputing waitbar/readbar/
// writebar/stall from scratch -- barrier-id assignment for VARIABLE-latency hazards is an
// interval-coloring problem over the 6 physical scoreboard slots (assignBarrierSlots()); every
// other hazard is closed by raising a stall count. If a kernel's instruction order needs more
// than 6 simultaneously-live scoreboard barriers, that reorder is provably unrepairable without
// inserting an instruction (forbidden -- the shuffler this tool supports only ever rearranges
// existing instructions), so that kernel's control codes are left unchanged and no output file is
// written. See correctControlCodes()'s own doc for the full design and its one known limitation
// (stall-count correction is a conservative lower bound, not an exact cycle model -- see
// simulateAndVerify()'s doc, same limitation verify-cc's verification already carries).
//
// SCOPE: Turing (sm_75) and newer only. Older architectures are rejected up front (see
// ccCommon.hpp's c_MinSupportedSMVersion) since the operand-role/latency data this tool needs
// is best documented from Turing onward.
//
// Examples:
//     correct-cc a.cubin
//         corrects a.cubin's control codes, writing a.ccubin
//
//     correct-cc a.cubin -o fixed.cubin
//         same, writing fixed.cubin
//
//     correct-cc a.cubin fixed.cubin
//         same as -o

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <elfio/elfio.hpp>

#include "../CuAsm/CuAsmLogger.hpp"
#include "../CuAsm/common.hpp"
#include "ccCommon.hpp"
#include "cliCommon.hpp"

namespace fs = std::filesystem;

namespace {

/**
 * @brief Reads a whole file into memory as raw bytes.
 * @param fname Path to read.
 * @return The file's contents.
 **/
std::vector<char> readWholeFile(const std::string& fname) {
    std::ifstream in(fname, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    return std::vector<char>(s.begin(), s.end());
}

/**
 * @brief Loads a cubin, decodes every kernel's instructions, runs real hazard-based control-code
 *        correction on each one (ccCommon.hpp's correctControlCodes(), Reports/tasks.md Phase 4),
 *        and -- only if every kernel was successfully repaired -- writes the result to a new
 *        cubin file by patching each kernel's ".text.<kernel>" bytes in place. Section sizes
 *        never change: correction only ever rewrites existing control-code fields, never adds/
 *        removes instructions.
 * @param fin Input cubin path.
 * @param fout Output cubin path.
 * @return True on success; false on any load, decode, correction (CheckStatus::Unrepairable), or
 *         re-merge-size-mismatch failure. On an Unrepairable kernel, no output file is written at
 *         all, rather than silently writing a cubin that still contains a real hazard.
 **/
bool correctCC(const std::string& fin, const std::string& fout) {
    ELFIO::elfio ef;
    if (!ef.load(fin)) {
        CuAsm::CuAsmLogger::logError("Failed to load ELF/cubin \"" + fin + "\"!");
        return false;
    }

    const auto arch = CuAsm::Tools::detectArch(ef);
    if (!arch) {
        CuAsm::CuAsmLogger::logError("Cubin \"" + fin +
                                      "\" targets an unsupported SM version -- correct-cc requires Turing "
                                      "(sm_75) or newer!");
        return false;
    }
    CuAsm::CuAsmLogger::logProcedure("Detected " + arch->getVersionString() + ".");

    std::vector<CuAsm::Tools::KernelControlCodes> kernels = CuAsm::Tools::loadControlCodes(ef, *arch);
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
        CuAsm::CuAsmLogger::logError(std::string("Failed to decode instructions for hazard correction: ") + e.what());
        return false;
    }

    std::vector<char> outBytes = readWholeFile(fin);
    bool anyUnrepairable = false;

    for (auto& kcc : kernels) {
        const CuAsm::Tools::ControlCodeCheckResult result =
            CuAsm::Tools::correctControlCodes(kcc, decodedByKernel.at(kcc.kernelName), *arch);
        switch (result.status) {
        case CuAsm::Tools::CheckStatus::Corrected:
            CuAsm::CuAsmLogger::logProcedure("Kernel \"" + kcc.kernelName + "\": CORRECTED -- " + result.message);
            break;
        case CuAsm::Tools::CheckStatus::Unrepairable:
            CuAsm::CuAsmLogger::logError("Kernel \"" + kcc.kernelName + "\": UNREPAIRABLE -- " + result.message);
            anyUnrepairable = true;
            break;
        case CuAsm::Tools::CheckStatus::Verified:
        case CuAsm::Tools::CheckStatus::Violated:
        case CuAsm::Tools::CheckStatus::NotImplemented:
            // correctControlCodes() never returns these -- they remain verifyControlCodes()-only,
            // handled here only so this switch stays exhaustive over the shared CheckStatus enum.
            CuAsm::CuAsmLogger::logProcedure("Kernel \"" + kcc.kernelName + "\": " + result.message);
            break;
        }

        const std::vector<std::byte> merged = arch->mergeCtrlCodes(kcc.insCodeList, kcc.ctrlCodeList);
        if (merged.size() != kcc.sectionSize) {
            CuAsm::CuAsmLogger::logError("Re-merged control codes for kernel \"" + kcc.kernelName +
                                          "\" changed section size (" + std::to_string(kcc.sectionSize) + " -> " +
                                          std::to_string(merged.size()) + " bytes)!");
            return false;
        }
        std::memcpy(outBytes.data() + kcc.sectionOffset, merged.data(), merged.size());
    }

    if (anyUnrepairable) {
        CuAsm::CuAsmLogger::logError("Not writing \"" + fout +
                                      "\": at least one kernel's control codes could not be repaired in place.");
        return false;
    }

    std::ofstream out(fout, std::ios::binary);
    out.write(outBytes.data(), static_cast<std::streamsize>(outBytes.size()));
    CuAsm::CuAsmLogger::logProcedure("Wrote corrected cubin to \"" + fout + "\".");

    return true;
}

} // namespace

/**
 * @brief Entry point for the correct-cc tool: parses arguments, sets up logging, and writes a
 *        hazard-corrected copy of a cubin's control codes.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success; -1 on any I/O, decode, or correction (CheckStatus::Unrepairable) error;
 *         CLI11's own exit code on a bad command line (see CLI11_PARSE).
 **/
int main(int argc, char** argv) {
    CLI::App app{"Correct scoreboard control codes of a cubin's instructions "
                 "(RAW/WAW/WAR hazard correction -- see Reports/control-codes-validation.md)."};

    std::vector<std::string> infiles;
    std::string outfile;
    std::string logfile;
    bool verbose = false;
    bool quiet = false;

    app.add_option("infiles", infiles, "Input file, optionally followed by an output file")->required()->expected(1, 2);
    CLI::Option* outOpt = app.add_option("-o,--output", outfile, "Output file");
    CLI::Option* logOpt = app.add_option("-f,--logfile", logfile, "Log file");
    app.add_flag("-v,--verbose", verbose, "Verbose output");
    app.add_flag("-q,--quiet", quiet, "Quiet output");

    CLI11_PARSE(app, argc, argv);

    const std::string infile = infiles[0];
    std::string outfileVal;
    if (infiles.size() == 2) {
        outfileVal = infiles[1];
    } else if (outOpt->count() > 0) {
        outfileVal = outfile;
    } else {
        outfileVal = fs::path(infile).replace_extension(".ccubin").string();
    }

    if (!checkInFileExistence(infile) || !checkOutFileBackup(outfileVal)) {
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

    return correctCC(infile, outfileVal) ? 0 : -1;
}
