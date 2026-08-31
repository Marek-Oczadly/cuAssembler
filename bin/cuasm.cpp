// Convert cubin from/to cuasm files.
//
// NOTE 1: if the output file already exists, the original file will be renamed to "outfile~".
// NOTE 2: if the logfile already exists, original logs will be rolled to logname.1, logname.2, until logname.3.
//
// Examples:
//     cuasm a.cubin
//         disassemble a.cubin => a.cuasm, text mostly inherited from nvdisasm. If output file name is not given,
//         the default name is replacing the ext to .cuasm
//
//     cuasm a.cuasm
//         assemble a.cuasm => a.cubin. If output file name is not given, default to replace the ext to .cubin
//
//     cuasm a.cubin -o x.cuasm
//         disassemble a.cubin => x.cuasm, specify the output file explicitly
//
//     cuasm a.cubin x.cuasm
//         same as `cuasm a.cubin -o x.cuasm`
//
//     cuasm a.o --bin2asm
//         disassemble a.o => a.cuasm, file type with extension ".o" is not recognized.
//         Thus conversion direction should be specified explicitly by "--bin2asm/--asm2bin".
//
//     cuasm a.cubin -f abc -v
//         disassemble a.cubin => a.cuasm, save log to abc.log, and verbose mode

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "../CuAsm/CuAsmLogger.hpp"
#include "CuAsmTools/Cuasm.hpp"
#include "cliCommon.hpp"

namespace fs = std::filesystem;

namespace {

/**
 * @brief Converts src to dst, choosing the cubin<->cuasm direction automatically from the src
 *        extension unless direction explicitly overrides it. Thin CLI wrapper around
 *        CuAsmTools/Cuasm.hpp's convert() -- handles the output-file backup check and prints a
 *        diagnostic on failure instead of throwing, since that's this CLI's own established
 *        contract (see cliCommon.hpp's checkOutFileBackup()).
 * @param src Path of the source file (.cubin/.bin or .cuasm/.asm, unless direction is given explicitly).
 * @param dst Optional path of the destination file; inferred from src if not given.
 * @param direction Conversion direction: Auto (infer from extension), Bin2Asm, or Asm2Bin.
 * @return True on success; false if the output file check failed or the conversion threw.
 **/
bool doProcess(const std::string& src, const std::optional<std::string>& dst, CuAsm::Tools::CuasmDirection direction) {
    const std::string fext = fs::path(src).extension().string();

    bool isBin2Asm;
    if (direction == CuAsm::Tools::CuasmDirection::Bin2Asm || fext == ".cubin" || fext == ".bin") {
        isBin2Asm = true;
    } else if (direction == CuAsm::Tools::CuasmDirection::Asm2Bin || fext == ".cuasm" || fext == ".asm") {
        isBin2Asm = false;
    } else {
        std::cout << "The first infile should be with ext \".cubin\" or \".cuasm\", "
                     "otherwise specify direction by option --bin2asm or --asm2bin!"
                  << std::endl;
        return false;
    }

    const std::string resolvedOutname = dst.value_or(fs::path(src).replace_extension(isBin2Asm ? ".cuasm" : ".cubin").string());

    if (!checkOutFileBackup(resolvedOutname)) {
        return false;
    }

    try {
        CuAsm::Tools::convert(src, resolvedOutname, direction);
    } catch (const std::exception& e) {
        std::cout << e.what() << std::endl;
        return false;
    }
    return true;
}

} // namespace

/**
 * @brief Entry point for the cuasm tool: parses arguments, sets up logging, and converts
 *        between cubin and cuasm as requested.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success; -1 on any I/O or conversion error; CLI11's own exit code on a bad
 *         command line (see CLI11_PARSE).
 **/
int main(int argc, char** argv) {
    CLI::App app{"Convert cubin from/to cuasm files."};

    std::vector<std::string> infiles;
    std::string outfile;
    std::string logfile;
    bool verbose = false;
    bool quiet = false;
    bool forceBin2Asm = false;
    bool forceAsm2Bin = false;

    app.add_option("infiles", infiles, "Input file, optionally followed by an output file")->required()->expected(1, 2);
    CLI::Option* outOpt = app.add_option("-o,--output", outfile, "Output file");
    CLI::Option* logOpt = app.add_option("-f,--logfile", logfile, "Log file");
    app.add_flag("-v,--verbose", verbose, "Verbose output");
    app.add_flag("-q,--quiet", quiet, "Quiet output");
    CLI::Option* bin2asmOpt = app.add_flag("--bin2asm", forceBin2Asm, "Force cubin -> cuasm direction");
    app.add_flag("--asm2bin", forceAsm2Bin, "Force cuasm -> cubin direction")->excludes(bin2asmOpt);

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

    CuAsm::Tools::CuasmDirection direction = CuAsm::Tools::CuasmDirection::Auto;
    if (forceBin2Asm) {
        direction = CuAsm::Tools::CuasmDirection::Bin2Asm;
    } else if (forceAsm2Bin) {
        direction = CuAsm::Tools::CuasmDirection::Asm2Bin;
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

    if (!doProcess(infile, outfileOpt, direction)) {
        return -1;
    }

    return 0;
}
