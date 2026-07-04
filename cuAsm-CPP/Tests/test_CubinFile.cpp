#include <filesystem>
#include <string>

#include "../CuAsm/CubinFile.hpp"

namespace fs = std::filesystem;

/**
 * @brief Replaces all occurrences of a substring within a string.
 * @param s String to search within.
 * @param from Substring to replace.
 * @param to Replacement substring.
 * @return The resulting string, with every occurrence of from replaced by to.
 **/
std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

/**
 * @brief Disassembles a cubin file and saves the result as a .cuasm file alongside it.
 * @param binname Path of the input cubin file.
 **/
void cubin2cuasm(const std::string& binname) {
    // print('Processing %s...' % binname)
    CuAsm::CubinFile cf(binname);
    // cf.initCuKernelAsm('CuAsm/InsAsmRepos.txt');
    // cf.loadCubin(binname);

    std::string asmname = replaceAll(binname, ".cubin", ".cuasm");
    // print('Saving to %s...' % asmname)
    cf.saveAsCuAsm(asmname);
}

/**
 * @brief Converts every .cubin file in the CuTest test data directory to .cuasm, mirroring the
 *        original Tests/test_CubinFile.py script.
 * @return 0 on success.
 **/
int main() {
    const std::string fdir = R"(G:\Work\CuAssembler\TestData\CuTest)";

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fdir, ec)) {
        if (entry.path().extension() == ".cubin") {
            cubin2cuasm(entry.path().string());
        }
    }

    return 0;
}
