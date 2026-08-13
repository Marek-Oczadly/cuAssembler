#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../CuAsm/CuAsmParser.hpp"
#include "../CuAsm/CuInsAssemblerRepos.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;

/**
 * @brief Runs fn and records a failure unless it throws exactly std::invalid_argument.
 * @param label Human-readable description of the operation, used in the pass/fail log line.
 * @param fn Operation expected to throw std::invalid_argument.
 **/
template <typename Fn>
void expectInvalidArgument(const std::string& label, Fn&& fn) {
    try {
        fn();
    } catch (const std::invalid_argument& e) {
        std::cout << "[PASS] " << label << " threw std::invalid_argument: " << e.what() << "\n";
        return;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << label << " threw the wrong exception type: " << e.what() << "\n";
        ++failures;
        return;
    }
    std::cerr << "[FAIL] " << label << " did not throw\n";
    ++failures;
}

/**
 * @brief Writes a single-line .cuasm fixture consisting of only an .__elf_flags directive, whose
 *        low byte encodes the target SM version - the same field real .cuasm files carry in their
 *        FileHeader block (see e.g. TestData/CuTest/cudatest.6.sm_61.cuasm's
 *        ".__elf_flags 0x3d053d"). CuAsmParser::Impl::preScan() dispatches this directive (and
 *        resolves the target CuSMVersion from it) as soon as it's encountered, before looking at
 *        anything else in the file, so a file containing just this one line is enough to reach
 *        that resolution and observe how it fails.
 * @param path Path to write the fixture to.
 * @param smVersion Numeric SM version to encode in the flags' low byte.
 **/
void writeMinimalCuAsm(const std::string& path, int smVersion) {
    std::ofstream f(path);
    f << ".__elf_flags 0x" << std::hex << smVersion << "\n";
}

} // namespace

/**
 * @brief Verifies that the two real entry points a caller would hit when targeting an
 *        unsupported architecture - parsing a .cuasm file, and requesting a default instruction
 *        repository directly - fail cleanly (a catchable std::invalid_argument) rather than
 *        crashing, hanging, or producing a silently-broken result.
 * @return 0 if both cases failed cleanly as expected, 1 otherwise.
 **/
int main() {
    const std::string cuasmPath = std::string(CUASM_TESTDATA_DIR) + "/CuTest/tmp_unsupported_arch.cuasm";
    writeMinimalCuAsm(cuasmPath, 89); // sm_89: no DefaultInsAsmRepos, no alias entry.

    expectInvalidArgument("CuAsmParser::parse() on a .cuasm targeting sm_89", [&cuasmPath] {
        CuAsm::CuAsmParser cap;
        cap.parse(cuasmPath);
    });

    std::error_code ec;
    fs::remove(cuasmPath, ec);

    expectInvalidArgument("CuInsAssemblerRepos::getDefaultRepos(\"sm_89\")",
                           [] { CuAsm::CuInsAssemblerRepos::getDefaultRepos("sm_89"); });

    return failures == 0 ? 0 : 1;
}
