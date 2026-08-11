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

#include "../CuAsm/CuAsmLogger.hpp"
#include "../CuAsm/utils/CubinUtils.hpp"

namespace fs = std::filesystem;

namespace {

struct Args {
    std::vector<std::string> infiles;
    std::optional<std::string> outfile;
    std::optional<std::string> logfile;
    bool verbose = false;
    bool quiet = false;
};

/**
 * @brief Verifies that an input file exists.
 * @param fname Path of the input file to check.
 * @return True if the file exists; false (after printing a diagnostic) otherwise.
 **/
bool checkInFileExistence(const std::string& fname) {
    if (!fs::is_regular_file(fname)) {
        std::cout << "IOError! Input file \"" << fname << "\" not found!" << std::endl;
        return false;
    }
    return true;
}

/**
 * @brief Checks whether an output file already exists and, if so, backs it up before it is overwritten.
 * @param fname Path of the output file to check.
 * @param doBackup If the file exists, rename it to "fname~" before returning; otherwise leave it untouched.
 * @return True on success; false (after printing a diagnostic) if fname is an existing directory.
 **/
bool checkOutFileBackup(const std::string& fname, bool doBackup = true) {
    if (fs::exists(fname)) {
        if (fs::is_directory(fname)) {
            std::cout << "IOError!!! Output file \"" << fname << "\" is an existing directory!" << std::endl;
            return false;
        } else {
            if (doBackup) {
                std::string bname = fname + "~";
                CuAsm::CuAsmLogger::logWarning("Backup existing file " + fname + " to " + bname + "...");
                fs::rename(fname, bname);
            }
        }
    }
    return true;
}

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

/**
 * @brief Prints the command-line usage message.
 * @param prog Program name to display in the usage message.
 **/
void printUsage(const std::string& prog) {
    std::cout << "Usage: " << prog << " infile [outfile] [-o outfile] [-f logfile] [-v|-q]" << std::endl;
}

/**
 * @brief Parses the command-line arguments, mirroring the original argparse-based CLI
 *        (positional infile[s], -o/--output, -f/--logfile, -v/--verbose, -q/--quiet).
 * @param argc Argument count, as passed to main().
 * @param argv Argument vector, as passed to main().
 * @return The parsed Args, with infiles containing either 1 (input only) or 2 (input, output) entries;
 *         std::nullopt if the arguments were invalid (usage has already been printed).
 **/
std::optional<Args> parseArgs(int argc, char** argv) {
    Args args;
    const std::string prog = argc > 0 ? fs::path(argv[0]).filename().string() : "hcubin";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                printUsage(prog);
                return std::nullopt;
            }
            args.outfile = argv[++i];
        } else if (arg == "-f" || arg == "--logfile") {
            if (i + 1 >= argc) {
                printUsage(prog);
                return std::nullopt;
            }
            args.logfile = argv[++i];
        } else if (arg == "-v" || arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "-q" || arg == "--quiet") {
            args.quiet = true;
        } else {
            args.infiles.push_back(arg);
        }
    }

    if (args.infiles.empty() || args.infiles.size() > 2) {
        std::cout << "The infile should be of length 1 (second infered by replacing file extension) or 2 !!!" << std::endl;
        return std::nullopt;
    }

    return args;
}

} // namespace

/**
 * @brief Entry point for the hcubin tool: parses arguments, sets up logging, and hacks the
 *        cache-policy desc bit of the requested cubin.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success; -1 on any argument, I/O, or hack error.
 **/
int main(int argc, char** argv) {
    std::optional<Args> argsOpt = parseArgs(argc, argv);
    if (!argsOpt.has_value()) {
        return -1;
    }
    Args& args = *argsOpt;

    std::string infile;
    std::optional<std::string> outfile;

    if (args.infiles.size() == 1) {
        infile = args.infiles[0];
        outfile = args.outfile;
    } else {
        infile = args.infiles[0];
        outfile = args.infiles[1];
    }

    if (!checkInFileExistence(infile)) {
        return -1;
    }

    int stdoutLevel;
    int fileLevel;
    if (args.verbose) {
        stdoutLevel = 0;
        fileLevel = 0;
    } else if (args.quiet) {
        stdoutLevel = static_cast<int>(CuAsm::LogLevel::Error);
        fileLevel = 25;
    } else {
        stdoutLevel = static_cast<int>(CuAsm::LogLevel::Procedure);
        fileLevel = static_cast<int>(CuAsm::LogLevel::Info);
    }

    if (args.logfile.has_value()) {
        CuAsm::CuAsmLogger::initLogger(args.logfile.value(), fileLevel, stdoutLevel);
    } else {
        CuAsm::CuAsmLogger::initLogger("", fileLevel, stdoutLevel, "cuasm", std::size_t(1) << 30, 3, false);
    }

    if (!hcubin(infile, outfile).has_value()) {
        return -1;
    }

    return 0;
}
