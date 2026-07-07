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

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "../CuAsm/CuAsmLogger.hpp"
#include "../CuAsm/CuInsFeeder.hpp"
#include "../CuAsm/common.hpp"
#include "../CuAsm/utils/CubinUtils.hpp"

namespace fs = std::filesystem;

namespace {

struct Args {
    std::vector<std::string> infiles;
    std::optional<std::string> outfile;
    std::optional<std::string> logfile;
    bool keepcode = false;
    bool nodeschack = false;
    bool verbose = false;
    bool quiet = false;
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
 * @brief Dumps sass (from a sass/cubin/binary file) annotated with scoreboard control codes.
 * @param fin Input filename: dumped sass (.sass), cubin (.cubin), or any other binary containing a cubin.
 * @param fout Optional output filename; defaults to fin with its extension replaced by ".dsass".
 * @param keepcode Keep code-only lines (e.g. SM5x/6x control code lines) in the output instead of stripping them.
 * @param noDescHack Skip the SM8x cache-policy desc-bit hack, no matter the cubin's SM version.
 **/
void dsass(const std::string& fin, std::optional<std::string> fout = std::nullopt, bool keepcode = false, bool noDescHack = false) {
    fs::path finPath(fin);
    std::string fext = finPath.extension().string();
    for (char& c : fext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string outname = fout.value_or(fs::path(finPath).replace_extension(".dsass").string());
    checkOutFileBackup(outname);

    std::string codeOnlyLineMode = keepcode ? "keep" : "none";

    if (fext == ".sass") {
        CuAsm::CuInsFeeder feeder(fin);
        CuAsm::CuAsmLogger::logEntry("Translating to " + outname + "...");
        feeder.trans(outname, codeOnlyLineMode);
    } else if (fext == ".dsass") {
        CuAsm::CuAsmLogger::logError("Input file \"" + fin + "\" is already a dsass file!!! Skipping...");
        std::exit(-1);
    } else if (fext == ".cubin") {
        std::string binname;
        std::string tmpname;
        bool doDescHack = false;
        std::string sass;

        try {
            if (noDescHack) {
                binname = fin;
            } else {
                tmpname = CuAsm::getTempFileName("", "cuasm", "cubin");
                doDescHack = CuAsm::fixCubinDesc(fin, tmpname);
                if (doDescHack) {
                    binname = tmpname;
                    CuAsm::CuAsmLogger::logWarning("Cubin " + fin + " needs desc hack!");
                } else {
                    binname = fin;
                }
            }

            CuAsm::CuAsmLogger::logEntry("Dumping sass from " + binname + "...");
            sass = CuAsm::checkOutput({"cuobjdump", "-sass", binname}, /*mergeStderr=*/false);
            if (doDescHack) {
                fs::remove(tmpname);
            }
        } catch (const CuAsm::CalledProcessError& cpe) {
            CuAsm::CuAsmLogger::logError(std::string("Error when running cuobjdump!") + cpe.output());
            std::exit(-1);
        } catch (const std::exception& e) {
            CuAsm::CuAsmLogger::logError(std::string("DumpSass Error!") + e.what());
            std::exit(-1);
        }

        std::istringstream sio(sass);
        CuAsm::CuInsFeeder feeder(sio);
        CuAsm::CuAsmLogger::logEntry("Translating to " + outname + " ...");
        feeder.trans(outname, codeOnlyLineMode);
    } else { // default: treat as an arbitrary binary containing a cubin
        std::string sass;

        try {
            CuAsm::CuAsmLogger::logEntry("Dumping sass from " + fin + "...");
            sass = CuAsm::checkOutput({"cuobjdump", "-sass", fin}, /*mergeStderr=*/false);
        } catch (const CuAsm::CalledProcessError& cpe) {
            CuAsm::CuAsmLogger::logError(std::string("Error when running cuobjdump!") + cpe.output());
            std::exit(-1);
        } catch (const std::exception& e) {
            CuAsm::CuAsmLogger::logError(std::string("DumpSass Error!") + e.what());
            std::exit(-1);
        }

        std::istringstream sio(sass);
        CuAsm::CuInsFeeder feeder(sio);
        CuAsm::CuAsmLogger::logEntry("Translating to " + outname + " ...");
        feeder.trans(outname, codeOnlyLineMode);
    }
}

/**
 * @brief Prints the command-line usage message and terminates the program.
 * @param prog Program name to display in the usage message.
 **/
void printUsageAndExit(const std::string& prog) {
    std::cout << "Usage: " << prog
              << " infile [outfile] [-o outfile] [-k] [-n] [-f logfile] [-v|-q]" << std::endl;
    std::exit(-1);
}

/**
 * @brief Parses the command-line arguments, mirroring the original argparse-based CLI
 *        (positional infile[s], -o/--output, -k/--keepcode, -n/--nodeschack, -f/--logfile,
 *        -v/--verbose, -q/--quiet).
 * @param argc Argument count, as passed to main().
 * @param argv Argument vector, as passed to main().
 * @return The parsed Args, with infiles containing either 1 (input only) or 2 (input, output) entries.
 **/
Args parseArgs(int argc, char** argv) {
    Args args;
    const std::string prog = argc > 0 ? fs::path(argv[0]).filename().string() : "dsass";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) printUsageAndExit(prog);
            args.outfile = argv[++i];
        } else if (arg == "-f" || arg == "--logfile") {
            if (i + 1 >= argc) printUsageAndExit(prog);
            args.logfile = argv[++i];
        } else if (arg == "-k" || arg == "--keepcode") {
            args.keepcode = true;
        } else if (arg == "-n" || arg == "--nodeschack") {
            args.nodeschack = true;
        } else if (arg == "-v" || arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "-q" || arg == "--quiet") {
            args.quiet = true;
        } else {
            args.infiles.push_back(arg);
        }
    }

    if (args.infiles.empty() || args.infiles.size() > 2) {
        std::cout << "The infile should be of length 1 or 2 (second as output)!!!" << std::endl;
        std::exit(-1);
    }

    return args;
}

} // namespace

/**
 * @brief Entry point for the dsass tool: parses arguments, sets up logging, and dumps
 *        control-code-annotated sass from the requested input.
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

    dsass(infile, outfile, args.keepcode, args.nodeschack);

    return 0;
}
