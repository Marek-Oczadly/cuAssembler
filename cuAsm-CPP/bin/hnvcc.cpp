// hnvcc is a hacked wrapper of nvcc.
//
// The operation depends on the environment variable 'HNVCC_OP':
//     Not-set or 'none' : call original nvcc
//     'dump' : dump cubins to hack.fname.sm_#.cubin, backup existing files.
//     'hack' : hack cubins with hack.fname.sm_#.cubin, skip if not exist
//     Others : error
//
// CAUTION:
//     hnvcc hack/dump need to append options "-keep"/"-keep-dir" to nvcc.
//     If these options are already in the option list, hnvcc may not work right.
//
// Examples:
//     hnvcc test.cu -arch=sm_75 -o test
//         call original nvcc
//
//     HNVCC_OP=dump hnvcc test.cu -arch=sm_75 -o test
//         dump test.sm_#.cubin to hack.test.sm_#.cubin
//
//     HNVCC_OP=hack hnvcc test.cu -arch=sm_75 -o test
//         hack test.sm_#.cubin with hack.test.sm_#.cubin

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../CuAsm/common.hpp"

namespace fs = std::filesystem;

namespace {

// Environment variable used to select the op (HNVCC_OP).
const std::string HNVCC_OP = "HNVCC_OP";

// Temporary dir for intermediate files, deleted after dump/hack (hnvcc_keep_dir).
const std::string KEEP_DIR = "hnvcc_keep_dir";

// Prefix for hacked cubins (hack).
const std::string HACK_PREFIX = "hack";

// Prefix for dumped cubins (dump).
const std::string DUMP_PREFIX = "dump";

const char* const USAGE_MSG = R"(
Usage: hnvcc args...

    hnvcc is the hacked wrapper of nvcc.
    The operation depends on the environment variable 'HNVCC_OP':
        Not-set or 'none' : call original nvcc
        'dump' : dump cubins to hack.fname.sm_#.cubin, backup existing files.
        'hack' : hack cubins with hack.fname.sm_#.cubin, skip if not exist
        Others : error

    CAUTION:
        hnvcc hack/dump need to append options "-keep"/"-keep-dir" to nvcc.
        If these options are already in option list, hnvcc may not work right.

    Examples:
        $ hnvcc test.cu -arch=sm_75 -o test
            call original nvcc

        $ HNVCC_OP=dump test.cu -arch=sm_75 -o test
            dump test.sm_#.cubin to hack.test.sm_#.cubin

        $ HNVCC_OP=hack test.cu -arch=sm_75 -o test
            hack test.sm_#.cubin with hack.test.sm_#.cubin
)";

/**
 * @brief Prints the hnvcc usage/help message.
 **/
void printUsage() {
    std::cout << USAGE_MSG << std::endl;
}

/**
 * @brief Strips leading and trailing whitespace from a string.
 * @param s String to trim.
 * @return The trimmed string.
 **/
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/**
 * @brief Converts a string to lowercase.
 * @param s String to convert.
 * @return The lowercased string.
 **/
std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/**
 * @brief Splits a shell command line into argv-style tokens, honoring single/double quotes,
 *        mirroring python's shlex.split().
 * @param line Command line to split.
 * @return The list of unquoted tokens.
 **/
std::vector<std::string> splitShellLine(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inToken = false;
    char quote = '\0';

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                cur += c;
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
            inToken = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (inToken) {
                tokens.push_back(cur);
                cur.clear();
                inToken = false;
            }
        } else {
            cur += c;
            inToken = true;
        }
    }
    if (inToken) tokens.push_back(cur);

    return tokens;
}

/**
 * @brief Runs a command, capturing its combined stdout/stderr, mirroring the original run()
 *        helper built on subprocess.check_output.
 * @param args Command and arguments, with args[0] as the executable name.
 * @param doCheck If true, throw when the command fails instead of just returning a negative code.
 * @return A pair of (return code, captured output); return code is 0 on success, -1 if the
 *         process exited with an error, -2 on any other failure to launch it.
 **/
std::pair<int, std::string> run(const std::vector<std::string>& args, bool doCheck = true) {
    std::pair<int, std::string> res;

    try {
        std::string out = CuAsm::checkOutput(args);
        res = {0, trim(out)};
    } catch (const CuAsm::CalledProcessError& cpe) {
        std::cout << "Error when running command" << std::endl;
        std::cout << cpe.output() << std::endl;
        res = {-1, ""};
    } catch (const std::exception& e) {
        std::cout << "Error when calling run(args)!" << std::endl;
        std::cout << e.what() << std::endl;
        res = {-2, ""};
    }

    if (doCheck && res.first != 0) {
        throw std::runtime_error("Check failed! Abort...");
    }

    return res;
}

/**
 * @brief Gets the cubin filename argument from a ptxas command line.
 * @param cmd Command and arguments (as produced by splitShellLine).
 * @return The last argument ending in ".cubin", or std::nullopt if none is found.
 **/
std::optional<std::string> getCubinArg(const std::vector<std::string>& cmd) {
    for (auto it = cmd.rbegin(); it != cmd.rend(); ++it) {
        if (it->size() >= 6 && it->compare(it->size() - 6, 6, ".cubin") == 0) {
            return *it;
        }
    }
    return std::nullopt;
}

/**
 * @brief Dry-runs nvcc to obtain its full command list, then either hacks each ptxas-produced
 *        cubin with a previously dumped "hack.*" cubin, or runs ptxas normally and dumps the
 *        resulting cubin as "dump.*" for later hacking.
 * @param args nvcc command-line arguments (args[0] is the compiler name, replaced with "nvcc").
 * @param op Either "hack" or "dump".
 **/
void doHackOrDump(const std::vector<std::string>& args, const std::string& op) {
    std::vector<std::string> argsDryrun = args;
    argsDryrun[0] = "nvcc";
    argsDryrun.push_back("-keep");  // Keep intermediate files
    argsDryrun.push_back("-keep-dir=" + KEEP_DIR); // Location for intermediate files
	argsDryrun.push_back("-dryrun"); // List compilation subcommands without executing them

    std::cout << "#### Getting command list..." << std::endl;
    auto [code, outS] = run(argsDryrun);
    if (outS.empty()) {
        throw std::runtime_error("Empty command list!!!");
    }

	static const std::regex varLinePattern(R"(^#\$ \w+=)"); // #$ _WORD_=

    std::vector<std::vector<std::string>> cmds;
    std::istringstream lines(outS);
    std::string line;
    while (std::getline(lines, line)) {
        std::string sline = trim(line);
		if (sline.empty() || sline.rfind("#$ ", 0) != 0 || std::regex_search(sline, varLinePattern)) { // Skip commands that are not #$ commands, or are variable assignments like "#$ _WORD_="
            continue;
        }

        bool isRmDlink = (sline.rfind("#$ rm", 0) == 0 || sline.rfind("#$ erase", 0) == 0) && // Skip commands that remove _dlink.reg.c files
                          sline.size() >= 12 && sline.compare(sline.size() - 12, 12, "_dlink.reg.c") == 0;
        if (isRmDlink) {
            continue;
        }

        cmds.push_back(splitShellLine(sline.substr(3)));
    }

    bool doDeleteKeepDir;
    if (fs::is_directory(KEEP_DIR)) {
        doDeleteKeepDir = false;
    } else {
        std::cout << "#### Creating keep dir " << KEEP_DIR << std::endl;
        doDeleteKeepDir = true;
        fs::create_directory(KEEP_DIR);
    }

    if (op == "hack") {
        for (const auto& cmd : cmds) {
            if (cmd[0] == "ptxas") {
                std::optional<std::string> fullname = getCubinArg(cmd);
                std::string cubinname = fs::path(*fullname).filename().string();
                std::string hackname = HACK_PREFIX + "." + cubinname;
                if (fs::is_regular_file(hackname)) {
                    std::cout << "#### Hacking with " << hackname << "..." << std::endl;
                    fs::copy_file(hackname, *fullname, fs::copy_options::overwrite_existing);
                } else {
                    std::cout << "#### Keeping " << cubinname << "..." << std::endl;
                    run(cmd);
                }
            } else {
                run(cmd);
            }
        }
    } else if (op == "dump") {
        // strip useless commands after last ptxas
        while (!cmds.empty() && cmds.back()[0] != "ptxas") {
            cmds.pop_back();
        }

        for (const auto& cmd : cmds) {
            if (cmd[0] == "ptxas") {
                std::optional<std::string> fullname = getCubinArg(cmd);
                std::string cubinname = fs::path(*fullname).filename().string();
                std::string dumpname = DUMP_PREFIX + "." + cubinname;
                if (fs::is_regular_file(dumpname)) {
                    fs::copy_file(dumpname, dumpname + "~", fs::copy_options::overwrite_existing);
                }

                run(cmd);
                std::cout << "#### Dumping " << dumpname << "..." << std::endl;
                fs::copy_file(*fullname, dumpname, fs::copy_options::overwrite_existing);
            } else {
                run(cmd);
            }
        }
    } else {
        throw std::runtime_error("Unknown op \"" + op + "\"");
    }

    if (doDeleteKeepDir) {
        std::cout << "#### Removing keep dir " << KEEP_DIR << "..." << std::endl;
        fs::remove_all(KEEP_DIR);
    }
}

/**
 * @brief Dispatches to a plain nvcc call, or to doHackOrDump(), based on the HNVCC_OP
 *        environment variable.
 * @param args Command-line arguments, with args[0] as the invoked program name (mutated to
 *        "nvcc" before running it).
 **/
void hnvcc(std::vector<std::string> args) {
    std::string op;
    const char* envOp = std::getenv(HNVCC_OP.c_str());
    if (envOp == nullptr) {
        op = "none";
    } else {
        op = toLower(trim(envOp));
        if (op != "hack" && op != "dump" && op != "none") {
            throw std::invalid_argument("Unknown HNVCC_OP \"" + op + "\"");
        }
    }

    if (op == "none") {
        args[0] = "nvcc";
        run(args);
    } else {
        doHackOrDump(args, op);
    }
}

} // namespace

/**
 * @brief Entry point for the hnvcc tool: prints usage, or dispatches to hnvcc() with the
 *        process's arguments.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success; -1 if run on Windows, where this tool is not supported.
 **/
int main(int argc, char** argv) {
#ifdef _WIN32
    std::cout << "Sorry! This tool (hnvcc) does not work right for windows..." << std::endl;
    return -1;
#else
    std::vector<std::string> args(argv, argv + argc);

    if (argc == 1 || (argc == 2 && args[1] == "-h")) {
        printUsage();
        return 0;
    }

    hnvcc(args);
    return 0;
#endif
}
