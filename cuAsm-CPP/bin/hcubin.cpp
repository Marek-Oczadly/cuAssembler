// Hack the sm8x cubin with valid cache-policy desc bit set.
//
// Currently the disassembly of nvdisasm will not show default cache-policy UR:
//
// /*00b0*/                   LDG.E R8, [R2.64] ;                      /* 0x0000000402087981 */
//                                                                     /* 0x000ea8000c1e1900 */
// /*00c0*/                   LDG.E R9, desc[UR6][R2.64+0x400] ;       /* 0x0004000602097981 */
//                                                                     /* 0x000ea8200c1e1900 */
//
// The first disassembly line should be `LDG.E R8, desc[UR4][R2.64] ;`,
// in which UR[4:5] is the default cache-policy UR and not showed, which may cause assembly confusion.
//
// But if the 102th bit(the "2" in last line 0x000ea8200c1e1900) is set,
// all cache-policy UR will be showed, that will complete the assembly input for the encoding.
//
// This tool sets that bit for every instruction that needs desc shown.
//
// Examples:
//     hcubin a.cubin
//         hack a.cubin into a.hcubin, default output name is replacing the ext to .hcubin
//
//     hcubin a.cubin -o x.bin
//         hack a.cubin into x.bin
//
//     hcubin a.cubin x.bin
//         same as `hcubin a.cubin -o x.bin`

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "../CuAsm/CuAsmLogger.hpp"
#include "../CuAsm/utils/CubinUtils.hpp"
#include "cliCommon.hpp"

namespace fs = std::filesystem;

namespace {

/**
 * @brief Hacks a cubin's cache-policy desc bit so it is shown explicitly wherever needed, writing
 *        the result to a new cubin file.
 * @param fin Path of the input cubin file.
 * @param fout Optional path of the output cubin file; defaults to fin with its extension replaced by ".hcubin".
 * @return Whether the cubin needed (and received) the desc hack (true) or its SM version predates
 *         Ampere and no hack was necessary (false) -- both are successful outcomes; std::nullopt if
 *         the input/output file checks failed.
 **/
std::optional<bool> hcubin(const std::string& fin, std::optional<std::string> fout = std::nullopt) {
    std::string outname = fout.value_or(fs::path(fin).replace_extension(".hcubin").string());

    if (!checkInFileExistence(fin) || !checkOutFileBackup(outname)) {
        return std::nullopt;
    }

    return CuAsm::fixCubinDesc(fin, outname);
}

} // namespace

/**
 * @brief Entry point for the hcubin tool: parses arguments, sets up logging, and hacks the
 *        cache-policy desc bit of the requested cubin.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success; -1 on any I/O or hack error; CLI11's own exit code on a bad command
 *         line (see CLI11_PARSE).
 **/
int main(int argc, char** argv) {
    CLI::App app{"Hack the sm8x cubin with valid cache-policy desc bit set."};

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

    std::string infile = infiles[0];
    std::optional<std::string> outfileOpt;
    if (infiles.size() == 2) {
        outfileOpt = infiles[1];
    } else if (outOpt->count() > 0) {
        outfileOpt = outfile;
    }

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

    if (!hcubin(infile, outfileOpt).has_value()) {
        return -1;
    }

    return 0;
}
