// Correct the scoreboard control codes of a cubin's instructions.
//
// CURRENT STATUS: this is a thin CLI skeleton. It loads a cubin, decodes every kernel's control
// codes, runs correctControlCodes() (ccCommon.hpp) on each one, re-merges the result, and
// writes a new cubin -- but correctControlCodes() is currently a no-op that leaves every control
// code unchanged. Real correction needs everything verify-cc's verification does, plus a
// barrier-slot (6 scoreboards) allocation solver and a reject/rollback path for reorders that
// can't be repaired without inserting instructions; see Reports/control-codes-validation.md for
// the full scope. Until that exists, the output cubin's ".text.<kernel>" sections are expected
// to be byte-identical to the input's -- this tool exercises the real split -> correct(stub) ->
// merge -> patch-into-file round trip end to end, ready for correctControlCodes() to be filled
// in without changing anything else.
//
// SCOPE: Turing (sm_75) and newer only. Older architectures are rejected up front (see
// ccCommon.hpp's c_MinSupportedSMVersion) since the operand-role/latency data this tool will
// eventually need is best documented from Turing onward.
//
// Examples:
//     correct-cc a.cubin
//         corrects a.cubin's control codes (currently a no-op), writing a.ccubin
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
#include <sstream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <elfio/elfio.hpp>

#include "../CuAsm/CuAsmLogger.hpp"
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
 * @brief Loads a cubin, runs (currently stubbed) control-code correction on each kernel, and
 *        writes the result to a new cubin file by patching each kernel's ".text.<kernel>" bytes
 *        in place. Section sizes never change: correction only ever rewrites existing
 *        control-code fields, never adds/removes instructions.
 * @param fin Input cubin path.
 * @param fout Output cubin path.
 * @return True on success; false on any load, decode, or re-merge-size-mismatch failure.
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

    std::vector<char> outBytes = readWholeFile(fin);

    for (auto& kcc : kernels) {
        const CuAsm::Tools::ControlCodeCheckResult result = CuAsm::Tools::correctControlCodes(kcc, *arch);
        CuAsm::CuAsmLogger::logProcedure("Kernel \"" + kcc.kernelName + "\": " + result.message);

        const std::vector<std::byte> merged = arch->mergeCtrlCodes(kcc.insCodeList, kcc.ctrlCodeList);
        if (merged.size() != kcc.sectionSize) {
            CuAsm::CuAsmLogger::logError("Re-merged control codes for kernel \"" + kcc.kernelName +
                                          "\" changed section size (" + std::to_string(kcc.sectionSize) + " -> " +
                                          std::to_string(merged.size()) + " bytes)!");
            return false;
        }
        std::memcpy(outBytes.data() + kcc.sectionOffset, merged.data(), merged.size());
    }

    std::ofstream out(fout, std::ios::binary);
    out.write(outBytes.data(), static_cast<std::streamsize>(outBytes.size()));
    CuAsm::CuAsmLogger::logProcedure("Wrote corrected cubin to \"" + fout + "\".");

    return true;
}

} // namespace

/**
 * @brief Entry point for the correct-cc tool: parses arguments, sets up logging, and writes a
 *        corrected (currently unchanged) copy of a cubin's control codes.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success; -1 on any I/O or decode error; CLI11's own exit code on a bad command
 *         line (see CLI11_PARSE).
 **/
int main(int argc, char** argv) {
    CLI::App app{"Correct scoreboard control codes of a cubin's instructions "
                 "(currently a no-op passthrough -- see Reports/control-codes-validation.md)."};

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
