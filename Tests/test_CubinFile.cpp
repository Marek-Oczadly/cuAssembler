#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../CuAsm/CubinFile.hpp"
#include "utils/TestUtilsCommon.hpp"

namespace fs = std::filesystem;

namespace {

std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

/**
 * @brief Exercises CuAsm::CubinFile against the real cudatest.6.sm_61.cubin fixture. Previously
 *        this file only disassembled every .cubin under TestData/CuTest with no assertion beyond
 *        "didn't throw"; a regression that silently produced wrong (but well-formed) disassembly
 *        would not have failed the build. The expected values below (toolkit version, virtual SM
 *        version, kernel/section names) were cross-checked against the fixture's own
 *        cuobjdump -elf output and its known-good .cuasm header comment
 *        (".__elf_flags 0x3d053d // Flags, SM_61(0x3d), COMPUTE_61(0x3d)"), not just re-derived
 *        from CubinFile itself.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    const std::string cubinPath = std::string(CUASM_TESTDATA_DIR) + "/CuTest/cudatest.6.sm_61.cubin";
    const std::string asmPath = std::string(CUASM_TESTDATA_DIR) + "/CuTest/tmp_test_cubinfile.cuasm";

    CuAsm::CubinFile cf(cubinPath);
    t.check("loading the fixture cubin resolves its arch/virtual-SM/toolkit version fields correctly",
            cf.m_Arch.has_value() && cf.m_Arch->getVersionNumber() == 61 && cf.m_VirtualSMVersion == 61 &&
                cf.m_ToolKitVersion == 111);

    cf.saveAsCuAsm(asmPath);
    t.check("saveAsCuAsm() writes a non-trivially-sized .cuasm file", fs::exists(asmPath) && fs::file_size(asmPath) > 1000);

    const std::string cuasm = readAll(asmPath);
    t.check("the generated .cuasm carries the same ELF flags as the source cubin (SM_61)",
            cuasm.find(".__elf_flags") != std::string::npos && cuasm.find("0x3d053d") != std::string::npos);
    t.check("the generated .cuasm declares .text sections for kernels known to be in this fixture "
            "(argtest, local_test, from the CuTest cudatest.cu source)",
            cuasm.find(".text._Z7argtestPiS_S_") != std::string::npos &&
                cuasm.find(".text._Z10local_testiiPi") != std::string::npos);
    t.check("the generated .cuasm includes .nv.info sections", cuasm.find(".nv.info") != std::string::npos);

    // Disassembling the same cubin twice must be deterministic.
    CuAsm::CubinFile cf2(cubinPath);
    const std::string asmPath2 = std::string(CUASM_TESTDATA_DIR) + "/CuTest/tmp_test_cubinfile2.cuasm";
    cf2.saveAsCuAsm(asmPath2);
    t.checkEqual("disassembling the same cubin twice produces byte-identical .cuasm output", readAll(asmPath2), cuasm);

    std::error_code ec;
    fs::remove(asmPath, ec);
    fs::remove(asmPath2, ec);

    return t.finish("test_CubinFile");
}
