#pragma once

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../CuAsm/common.hpp"

// Clean C++ entry points for the `hnvcc` CLI tool (bin/hnvcc.cpp) -- a Linux-only nvcc wrapper
// that dry-runs nvcc to either dump a kernel's ptxas-produced cubin (for later hand/tool patching)
// or hack a previously-dumped cubin back into a fresh build in place of ptxas's own output, e.g.
// to substitute correct-cc's corrected control codes into a real build without touching the build
// system. bin/hnvcc.cpp dispatches on the HNVCC_OP environment variable and mutates argv[0]; these
// entry points take the operation and nvcc argument list explicitly instead, so they can be called
// from C++ without touching the process environment -- runNvccFromEnvironment() is provided
// separately for exact CLI-argv0/env-var parity with bin/hnvcc.cpp itself.
//
// Windows: hnvcc has never supported Windows (see bin/hnvcc.cpp's own #ifdef _WIN32 guard). These
// entry points throw std::runtime_error immediately there instead of only compiling out, so a
// caller gets a clear error rather than a missing-symbol link failure when this header is included
// on Windows.

namespace CuAsm::Tools {

#ifdef _WIN32

[[noreturn]] inline void runNvcc(std::vector<std::string> /*args*/) {
    throw std::runtime_error("runNvcc: hnvcc is not supported on Windows");
}
[[noreturn]] inline void dumpNvccCubins(std::vector<std::string> /*args*/) {
    throw std::runtime_error("dumpNvccCubins: hnvcc is not supported on Windows");
}
[[noreturn]] inline void hackNvccCubins(std::vector<std::string> /*args*/) {
    throw std::runtime_error("hackNvccCubins: hnvcc is not supported on Windows");
}
[[noreturn]] inline void runNvccFromEnvironment(std::vector<std::string> /*args*/) {
    throw std::runtime_error("runNvccFromEnvironment: hnvcc is not supported on Windows");
}

#else

namespace detail {

namespace fs = std::filesystem;

inline constexpr const char* c_KeepDir = "hnvcc_keep_dir";
inline constexpr const char* c_HackPrefix = "hack";
inline constexpr const char* c_DumpPrefix = "dump";

/**
 * @brief Splits a shell command line into argv-style tokens, honoring single/double quotes,
 *        mirroring python's shlex.split() (and bin/hnvcc.cpp's identically-named helper).
 * @param line Command line to split.
 * @return The list of unquoted tokens.
 **/
inline std::vector<std::string> splitShellLine(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inToken = false;
    char quote = '\0';

    for (std::size_t i = 0; i < line.size(); ++i) {
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
 * @brief Runs a command, capturing its combined stdout/stderr.
 * @param args Command and arguments, with args[0] as the executable name.
 * @return The captured, trimmed output.
 * @throws std::runtime_error if the command fails to launch or exits non-zero.
 **/
inline std::string runCaptured(const std::vector<std::string>& args) {
    try {
        return trim(checkOutput(args));
    } catch (const CalledProcessError& cpe) {
        throw std::runtime_error("hnvcc: command failed: " + cpe.output());
    }
}

/**
 * @brief Gets the cubin filename argument from a ptxas command line.
 * @param cmd Command and arguments (as produced by splitShellLine()).
 * @return The last argument ending in ".cubin", or std::nullopt if none is found.
 **/
inline std::optional<std::string> getCubinArg(const std::vector<std::string>& cmd) {
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
 *        resulting cubin as "dump.*" for later hacking. Mirrors bin/hnvcc.cpp's doHackOrDump().
 * @param args nvcc command-line arguments (args[0] is replaced with "nvcc").
 * @param op Either "hack" or "dump".
 * @throws std::runtime_error if the dry run produces no command list, or op is neither "hack" nor "dump".
 **/
inline void hackOrDumpNvccCubins(std::vector<std::string> args, const std::string& op) {
    std::vector<std::string> argsDryrun = std::move(args);
    argsDryrun[0] = "nvcc";
    argsDryrun.push_back("-keep");
    argsDryrun.push_back(std::string("-keep-dir=") + c_KeepDir);
    argsDryrun.push_back("-dryrun");

    const std::string outS = runCaptured(argsDryrun);
    if (outS.empty()) {
        throw std::runtime_error("hackOrDumpNvccCubins: empty command list from nvcc -dryrun");
    }

    static const std::regex varLinePattern(R"(^#\$ \w+=)");

    std::vector<std::vector<std::string>> cmds;
    std::istringstream lines(outS);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string sline = trim(line);
        if (sline.empty() || sline.rfind("#$ ", 0) != 0 || std::regex_search(sline, varLinePattern)) {
            continue;
        }

        const bool isRmDlink = (sline.rfind("#$ rm", 0) == 0 || sline.rfind("#$ erase", 0) == 0) &&
                                sline.size() >= 12 && sline.compare(sline.size() - 12, 12, "_dlink.reg.c") == 0;
        if (isRmDlink) {
            continue;
        }

        cmds.push_back(splitShellLine(sline.substr(3)));
    }

    bool doDeleteKeepDir;
    if (fs::is_directory(c_KeepDir)) {
        doDeleteKeepDir = false;
    } else {
        doDeleteKeepDir = true;
        fs::create_directory(c_KeepDir);
    }

    if (op == "hack") {
        for (const auto& cmd : cmds) {
            if (cmd[0] == "ptxas") {
                const std::optional<std::string> fullname = getCubinArg(cmd);
                const std::string cubinname = fs::path(*fullname).filename().string();
                const std::string hackname = std::string(c_HackPrefix) + "." + cubinname;
                if (fs::is_regular_file(hackname)) {
                    fs::copy_file(hackname, *fullname, fs::copy_options::overwrite_existing);
                } else {
                    runCaptured(cmd);
                }
            } else {
                runCaptured(cmd);
            }
        }
    } else if (op == "dump") {
        while (!cmds.empty() && cmds.back()[0] != "ptxas") {
            cmds.pop_back();
        }

        for (const auto& cmd : cmds) {
            if (cmd[0] == "ptxas") {
                const std::optional<std::string> fullname = getCubinArg(cmd);
                const std::string cubinname = fs::path(*fullname).filename().string();
                const std::string dumpname = std::string(c_DumpPrefix) + "." + cubinname;
                if (fs::is_regular_file(dumpname)) {
                    fs::copy_file(dumpname, dumpname + "~", fs::copy_options::overwrite_existing);
                }

                runCaptured(cmd);
                fs::copy_file(*fullname, dumpname, fs::copy_options::overwrite_existing);
            } else {
                runCaptured(cmd);
            }
        }
    } else {
        throw std::runtime_error("hackOrDumpNvccCubins: unknown op \"" + op + "\"");
    }

    if (doDeleteKeepDir) {
        fs::remove_all(c_KeepDir);
    }
}

} // namespace detail

/**
 * @brief Runs nvcc directly with the given arguments (HNVCC_OP="none" behavior).
 * @param args nvcc command-line arguments; args[0] (the invoked program name, if present) is
 *        replaced with "nvcc" before running.
 * @throws std::runtime_error if nvcc fails to launch or exits non-zero.
 **/
inline void runNvcc(std::vector<std::string> args) {
    if (args.empty()) {
        args.emplace_back("nvcc");
    } else {
        args[0] = "nvcc";
    }
    detail::runCaptured(args);
}

/**
 * @brief Dry-runs nvcc and dumps each ptxas-produced cubin to "dump.<cubin>" for later hacking
 *        (HNVCC_OP="dump" behavior). Requires "-keep"/"-keep-dir" not already be present in `args`.
 * @param args nvcc command-line arguments (args[0] is replaced with "nvcc").
 * @throws std::runtime_error on a dry-run/build failure.
 **/
inline void dumpNvccCubins(std::vector<std::string> args) {
    detail::hackOrDumpNvccCubins(std::move(args), "dump");
}

/**
 * @brief Dry-runs nvcc and, for each ptxas-produced cubin, substitutes a previously dumped
 *        "hack.<cubin>" in its place if one exists, otherwise builds normally (HNVCC_OP="hack"
 *        behavior). Requires "-keep"/"-keep-dir" not already be present in `args`.
 * @param args nvcc command-line arguments (args[0] is replaced with "nvcc").
 * @throws std::runtime_error on a dry-run/build failure.
 **/
inline void hackNvccCubins(std::vector<std::string> args) {
    detail::hackOrDumpNvccCubins(std::move(args), "hack");
}

/**
 * @brief Dispatches to runNvcc()/dumpNvccCubins()/hackNvccCubins() based on the HNVCC_OP
 *        environment variable, mirroring bin/hnvcc.cpp's own hnvcc() function exactly (including
 *        argv[0] mutation) -- provided for CLI parity, not needed by callers that already know
 *        which operation they want.
 * @param args Command-line arguments, with args[0] as the invoked program name.
 * @throws std::invalid_argument if HNVCC_OP is set to something other than "hack"/"dump"/"none"/unset.
 * @throws std::runtime_error on a dry-run/build failure.
 **/
inline void runNvccFromEnvironment(std::vector<std::string> args) {
    std::string op;
    const char* envOp = std::getenv("HNVCC_OP");
    if (envOp == nullptr) {
        op = "none";
    } else {
        op = trim(envOp);
        for (char& c : op) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (op != "hack" && op != "dump" && op != "none") {
            throw std::invalid_argument("runNvccFromEnvironment: unknown HNVCC_OP \"" + op + "\"");
        }
    }

    if (op == "none") {
        runNvcc(std::move(args));
    } else {
        detail::hackOrDumpNvccCubins(std::move(args), op);
    }
}

#endif

} // namespace CuAsm::Tools
