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
//
// The actual op dispatch/dry-run/dump/hack logic lives in CuAsmTools/Hnvcc.hpp (see that header's
// runNvccFromEnvironment(), which this main() calls directly) -- this file is now just the CLI
// entry point (usage text, argv plumbing, the Windows-unsupported guard).

#include <iostream>
#include <string>
#include <vector>

#include "CuAsmTools/Hnvcc.hpp"

namespace {

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

} // namespace

/**
 * @brief Entry point for the hnvcc tool: prints usage, or dispatches to
 *        CuAsm::Tools::runNvccFromEnvironment() with the process's arguments.
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

    CuAsm::Tools::runNvccFromEnvironment(args);
    return 0;
#endif
}
