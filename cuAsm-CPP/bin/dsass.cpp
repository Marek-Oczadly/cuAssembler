// Format sass with control codes from input sass/cubin/exe/...
//
// The original dumped sass by `cuobjdump -sass *.exe` will not show scoreboard control codes,
// which make it obscure to inspect the dependencies of instructions.
// This tool extracts the scoreboard info and shows it alongside the original disassembly.
//
// CAUTION: the sass input should be in exactly the format of `cuobjdump -sass`, otherwise
//          the parser may not work correctly.
//
// NOTE 1: For cubins of sm8x, the cache-policy desc bit of some instructions will be set to 1
//         to show desc[UR#] explicitly, other type of inputs(sass/exe/...) won't do the hack,
//         which means some instructions may not be assembled normally as in cuasm files.
//         This also implies for desc hacked sass, code of instructions may be not consistent either.
//
// NOTE 2: if the output file already exists, the original file will be renamed to "outfile~".
// NOTE 3: if the logfile already exists, original logs will be rolled to log.1, log.2, until log.3.
//
// Examples:
//     dsass a.cubin
//         dump sass from a.cubin, and write the result with control code to a.dsass
//
//     dsass a.exe -o a.txt
//         dump sass from a.cubin, and write the result with control code to a.txt
//
//     dsass a.sass
//         translate the cuobjdumped sass into a.dsass
//
//     dsass a.cubin -f abc -v
//         convert a.cubin => a.dsass, save log to abc.log, and verbose mode
//
//     dsass a.cubin -k
//         usually lines with only codes in source sass will be ignored for compact output.
//         use option -k/--keepcode to keep those lines.

#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "../CuAsm/CuAsmLogger.hpp"
#include "../CuAsm/common.hpp"
#include "CuAsmTools/Dsass.hpp"
#include "cliCommon.hpp"

namespace fs = std::filesystem;

namespace {

/**
 * @brief Dumps sass (from a sass/cubin/binary file) annotated with scoreboard control codes. Thin
 *        CLI wrapper around CuAsmTools/Dsass.hpp's dumpControlCodeSass() -- handles the output-
 *        file backup check and logs a diagnostic on failure instead of throwing, since that's this
 *        CLI's own established contract (see cliCommon.hpp's checkOutFileBackup()).
 * @param fin Input filename: dumped sass (.sass), cubin (.cubin), or any other binary containing a cubin.
 * @param fout Optional output filename; defaults to fin with its extension replaced by ".dsass".
 * @param keepcode Keep code-only lines (e.g. SM5x/6x control code lines) in the output instead of stripping them.
 * @param noDescHack Skip the SM8x cache-policy desc-bit hack, no matter the cubin's SM version.
 * @return True on success; false if the output file check failed, the input was already a dsass
 *         file, or dumping/translating the sass failed.
 **/
bool dsass(const std::string& fin, std::optional<std::string> fout = std::nullopt, bool keepcode = false, bool noDescHack = false) {
    const std::string outname = fout.value_or(fs::path(fin).replace_extension(".dsass").string());
    if (!checkOutFileBackup(outname)) {
        return false;
    }

    try {
        CuAsm::CuAsmLogger::logEntry("Translating to " + outname + "...");
        CuAsm::Tools::dumpControlCodeSass(fin, outname, keepcode, noDescHack);
    } catch (const CuAsm::CalledProcessError& cpe) {
        CuAsm::CuAsmLogger::logError(std::string("Error when running cuobjdump!") + cpe.output());
        return false;
    } catch (const std::exception& e) {
        CuAsm::CuAsmLogger::logError(std::string("DumpSass Error!") + e.what());
        return false;
    }

    return true;
}

} // namespace

/**
 * @brief Entry point for the dsass tool: parses arguments, sets up logging, and dumps
 *        control-code-annotated sass from the requested input.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success; -1 on any I/O or dumping error; CLI11's own exit code on a bad
 *         command line (see CLI11_PARSE).
 **/
int main(int argc, char** argv) {
    CLI::App app{"Format sass with control codes from input sass/cubin/exe/..."};

    std::vector<std::string> infiles;
    std::string outfile;
    std::string logfile;
    bool keepcode = false;
    bool nodeschack = false;
    bool verbose = false;
    bool quiet = false;

    app.add_option("infiles", infiles, "Input file, optionally followed by an output file")->required()->expected(1, 2);
    CLI::Option* outOpt = app.add_option("-o,--output", outfile, "Output file");
    CLI::Option* logOpt = app.add_option("-f,--logfile", logfile, "Log file");
    app.add_flag("-k,--keepcode", keepcode, "Keep code-only lines in the output instead of stripping them");
    app.add_flag("-n,--nodeschack", nodeschack, "Skip the SM8x cache-policy desc-bit hack");
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

    if (!dsass(infile, outfileOpt, keepcode, nodeschack)) {
        return -1;
    }

    return 0;
}
