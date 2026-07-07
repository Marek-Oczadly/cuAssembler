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

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "../CuAsm/CuAsmLogger.hpp"
#include "../CuAsm/CuAsmParser.hpp"
#include "../CuAsm/CubinFile.hpp"

namespace fs = std::filesystem;

namespace {

enum class Direction {
    Auto,
    Bin2Asm,
    Asm2Bin,
};

struct Args {
    std::vector<std::string> infiles;
    std::optional<std::string> outfile;
    std::optional<std::string> logfile;
    bool verbose = false;
    bool quiet = false;
    Direction direction = Direction::Auto;
};

/**
 * @brief Verifies that an input file exists, aborting the program otherwise.
 * @param fname Path of the input file to check.
 **/
void checkInFileExistence(const std::string& fname) {
    if (!fs::is_regular_file(fname)) {
        std::cout << "IOError! Input file \"" << fname << "\" not found!" << std::endl;
        std::exit(-1);
    }
}

/**
 * @brief Checks whether an output file already exists and, if so, backs it up before it is overwritten.
 * @param fname Path of the output file to check.
 * @param doBackup If the file exists, rename it to "fname~" before returning; otherwise leave it untouched.
 **/
void checkOutFileBackup(const std::string& fname, bool doBackup = true) {
    if (fs::exists(fname)) {
        if (fs::is_directory(fname)) {
            std::cout << "IOError!!! Output file \"" << fname << "\" is an existing directory!" << std::endl;
            std::exit(-1);
        } else {
            if (doBackup) {
                std::string bname = fname + "~";
                CuAsm::CuAsmLogger::logWarning("Backup existing file " + fname + " to " + bname + "...");
                fs::rename(fname, bname);
            }
        }
    }
}

/**
 * @brief Disassembles a cubin file and writes the result as a cuasm file.
 * @param binname Path of the input cubin file.
 * @param asmname Path of the output cuasm file; if not given, defaults to binname with its extension replaced by ".cuasm".
 **/
void cubin2cuasm(const std::string& binname, std::optional<std::string> asmname = std::nullopt) {
    std::string outname = asmname.value_or(fs::path(binname).replace_extension(".cuasm").string());

    CuAsm::CubinFile cf(binname);
    checkOutFileBackup(outname);
    cf.saveAsCuAsm(outname);
}

/**
 * @brief Assembles a cuasm file and writes the result as a cubin file.
 * @param asmname Path of the input cuasm file.
 * @param binname Path of the output cubin file; if not given, defaults to asmname with its extension replaced by ".cubin".
 **/
void cuasm2cubin(const std::string& asmname, std::optional<std::string> binname = std::nullopt) {
    CuAsm::CuAsmParser cap;
    cap.parse(asmname);

    std::string outname = binname.value_or(fs::path(asmname).replace_extension(".cubin").string());

    checkOutFileBackup(outname);
    cap.saveAsCubin(outname);
}

/**
 * @brief Converts src to dst, choosing the cubin<->cuasm direction automatically from the src extension
 *        unless direction explicitly overrides it.
 * @param src Path of the source file (.cubin/.bin or .cuasm/.asm, unless direction is given explicitly).
 * @param dst Optional path of the destination file; inferred from src if not given.
 * @param direction Conversion direction: Auto (infer from extension), Bin2Asm, or Asm2Bin.
 **/
void doProcess(const std::string& src, const std::optional<std::string>& dst, Direction direction) {
    std::string fext = fs::path(src).extension().string();

    if (direction == Direction::Bin2Asm || fext == ".cubin" || fext == ".bin") {
        cubin2cuasm(src, dst);
    } else if (direction == Direction::Asm2Bin || fext == ".cuasm" || fext == ".asm") {
        cuasm2cubin(src, dst);
    } else {
        std::cout << "The first infile should be with ext \".cubin\" or \".cuasm\", "
                     "otherwise specify direction by option --bin2asm or --asm2bin!"
                  << std::endl;
        std::exit(-1);
    }
}

/**
 * @brief Prints the command-line usage message and terminates the program.
 * @param prog Program name to display in the usage message.
 **/
void printUsageAndExit(const std::string& prog) {
    std::cout << "Usage: " << prog << " infile [outfile] [-o outfile] [-f logfile] [-v|-q] [--bin2asm|--asm2bin]"
              << std::endl;
    std::exit(-1);
}

/**
 * @brief Parses the command-line arguments, mirroring the original argparse-based CLI
 *        (positional infile[s], -o/--output, -f/--logfile, -v/--verbose, -q/--quiet,
 *        --bin2asm, --asm2bin).
 * @param argc Argument count, as passed to main().
 * @param argv Argument vector, as passed to main().
 * @return The parsed Args, with infiles containing either 1 (input only) or 2 (input, output) entries.
 **/
Args parseArgs(int argc, char** argv) {
    Args args;
    const std::string prog = argc > 0 ? fs::path(argv[0]).filename().string() : "cuasm";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) printUsageAndExit(prog);
            args.outfile = argv[++i];
        } else if (arg == "-f" || arg == "--logfile") {
            if (i + 1 >= argc) printUsageAndExit(prog);
            args.logfile = argv[++i];
        } else if (arg == "-v" || arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "-q" || arg == "--quiet") {
            args.quiet = true;
        } else if (arg == "--bin2asm") {
            args.direction = Direction::Bin2Asm;
        } else if (arg == "--asm2bin") {
            args.direction = Direction::Asm2Bin;
        } else {
            args.infiles.push_back(arg);
        }
    }

    if (args.infiles.empty() || args.infiles.size() > 2) {
        std::cout << "The infile should be of length 1 or 2 (second as output) !!!" << std::endl;
        std::exit(-1);
    }

    return args;
}

} // namespace

/**
 * @brief Entry point for the cuasm tool: parses arguments, sets up logging, and converts
 *        between cubin and cuasm as requested.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success; the process exits early with -1 on any argument or I/O error.
 **/
int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    std::string infile;
    std::optional<std::string> outfile;

    if (args.infiles.size() == 1) {
        infile = args.infiles[0];
        outfile = args.outfile;
    } else {
        infile = args.infiles[0];
        outfile = args.infiles[1];
    }

    checkInFileExistence(infile);

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

    doProcess(infile, outfile, args.direction);

    return 0;
}
