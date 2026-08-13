#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "../../CuAsm/utils/CubinUtils.hpp"
#include "TestUtilsCommon.hpp"

namespace fs = std::filesystem;

namespace {

/** @brief Reads an entire file back in, for content/size checks. */
std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

/**
 * @brief Exercises the parts of CuAsm::utils::CubinUtils that operate purely on local files -
 *        no nvcc/nvdisasm/cuobjdump subprocess required - against the real cubin fixtures under
 *        TestData/CuTest/. hackCubinDesc/fixCubinDesc/demoteDefaultDescBits all early-exit for
 *        any pre-Ampere (SM major version < 8) cubin before ever shelling out, which both fixture
 *        cubins (sm_61, sm_75) are, so their early-exit branch - previously entirely untested -
 *        can be verified without a CUDA toolkit installed.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    const std::string testDir = std::string(CUASM_TESTDATA_DIR) + "/CuTest";
    const std::string realCubin = testDir + "/cudatest.6.sm_61.cubin"; // sm_61: pre-Ampere (SM major 6 < 8).

    t.check("pShowDesc is the documented single high bit (1 << 101)", CuAsm::pShowDesc == (CuAsm::BigInt(1) << 101));

    // ---- transPTXVersion: pure text substitution, no subprocess ----

    const std::string ptxIn = testDir + "/tmp_test_cubinutils.ptx";
    const std::string ptxOut = testDir + "/tmp_test_cubinutils.out.ptx";
    {
        std::ofstream f(ptxIn);
        f << ".version 7.0\n.target sm_70, debug\n.address_size 64\n\n// a comment line\n";
    }
    CuAsm::transPTXVersion(ptxIn, ptxOut, "sm_86", "8.5");
    const std::string ptxContent = readAll(ptxOut);
    t.check("transPTXVersion rewrites .version/.target and leaves other lines untouched",
            ptxContent.find(".version 8.5") != std::string::npos && ptxContent.find(".target sm_86") != std::string::npos &&
                ptxContent.find(".address_size 64") != std::string::npos && ptxContent.find("// a comment line") != std::string::npos);
    t.checkThrows<std::runtime_error>("transPTXVersion on a nonexistent .ptx file throws cleanly",
                                       [&] { CuAsm::transPTXVersion(testDir + "/tmp_no_such.ptx", ptxOut); });

    // ---- feedBinFromCubin: extracts .text.* sections straight from ELF bytes, no subprocess ----

    const std::string binOut = testDir + "/tmp_test_cubinutils_feed.bin";
    int perKernelCallbacks = 0;
    CuAsm::feedBinFromCubin(realCubin, [&](const std::string&) { ++perKernelCallbacks; }, binOut, /*mergeAllKernels=*/false);
    const bool perKernelWroteData = perKernelCallbacks > 0 && !readAll(binOut).empty();

    int mergedCallbacks = 0;
    CuAsm::feedBinFromCubin(realCubin, [&](const std::string&) { ++mergedCallbacks; }, binOut, /*mergeAllKernels=*/true);
    t.check("feedBinFromCubin invokes its callback once per kernel by default, and once total when "
            "merging, in both cases with real extracted .text bytes",
            perKernelWroteData && mergedCallbacks == 1 && !readAll(binOut).empty());

    // ---- hackCubinDesc: pre-Ampere early exit ----

    const std::string hackOut = testDir + "/tmp_test_cubinutils_hack.cubin";
    std::error_code ec;
    fs::remove(hackOut, ec);
    const bool hackedWithAlwaysOutput = CuAsm::hackCubinDesc(realCubin, hackOut, /*alwaysOutput=*/true);
    const bool copiedOnAlwaysOutput = fs::exists(hackOut) && readAll(hackOut) == readAll(realCubin);
    fs::remove(hackOut, ec);
    const bool hackedWithoutAlwaysOutput = CuAsm::hackCubinDesc(realCubin, hackOut, /*alwaysOutput=*/false);
    t.check("hackCubinDesc on a pre-Ampere cubin reports no hack needed, only copying the file when "
            "alwaysOutput requests it",
            !hackedWithAlwaysOutput && copiedOnAlwaysOutput && !hackedWithoutAlwaysOutput && !fs::exists(hackOut));

    // ---- fixCubinDesc / demoteDefaultDescBits: pre-Ampere early exit ----

    const std::string fixOut = testDir + "/tmp_test_cubinutils_fix.cubin";
    fs::remove(fixOut, ec);
    const bool fixed = CuAsm::fixCubinDesc(realCubin, fixOut);

    const std::string demoteCopy = testDir + "/tmp_test_cubinutils_demote.cubin";
    fs::copy_file(realCubin, demoteCopy, fs::copy_options::overwrite_existing);
    const std::string demoteBefore = readAll(demoteCopy);
    const bool demoted = CuAsm::demoteDefaultDescBits(demoteCopy);
    t.check("fixCubinDesc/demoteDefaultDescBits on a pre-Ampere cubin report no fix needed, without "
            "writing any output (fixCubinDesc) or mutating the file in place (demoteDefaultDescBits)",
            !fixed && !fs::exists(fixOut) && !demoted && readAll(demoteCopy) == demoteBefore);

    // ---- error paths shared by hackCubinDesc/fixCubinDesc/demoteDefaultDescBits ----

    t.checkThrows<std::runtime_error>("hackCubinDesc on a nonexistent cubin throws cleanly",
                                       [&] { (void)CuAsm::hackCubinDesc(testDir + "/tmp_no_such.cubin", hackOut); });

    const std::string tinyFile = testDir + "/tmp_test_cubinutils_tiny.cubin";
    {
        std::ofstream f(tinyFile, std::ios::binary);
        f.write("\0\0\0\0\0\0\0\0", 8); // far smaller than an ELF header
    }
    t.checkThrows<std::runtime_error>("hackCubinDesc on a file too small to be an ELF header throws cleanly",
                                       [&] { (void)CuAsm::hackCubinDesc(tinyFile, hackOut); });

    fs::remove(ptxIn, ec);
    fs::remove(ptxOut, ec);
    fs::remove(binOut, ec);
    fs::remove(hackOut, ec);
    fs::remove(fixOut, ec);
    fs::remove(demoteCopy, ec);
    fs::remove(tinyFile, ec);

    return t.finish("test_CubinUtils");
}
