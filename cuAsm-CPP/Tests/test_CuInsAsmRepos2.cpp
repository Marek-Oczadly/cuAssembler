#include <string>

#include "../CuAsm/CuInsAssemblerRepos.hpp"

/**
 * @brief Loads an sm_75 instruction-assembler repos, merges in a second repos file, completes
 *        predicate codes, and saves the merged result, mirroring the original
 *        Tests/test_CuInsAsmRepos2.py script.
 * @return 0 on success.
 **/
int main() {
    std::string sassname = std::string(CUASM_TESTDATA_DIR) + "/CuTest/cudatest.sm_75.sass";
    // std::string sassname = std::string(CUASM_TESTDATA_DIR) + "/CuTest/cudnn64_7.sm_50.sass";
    std::string reposfile = std::string(CUASM_TESTDATA_DIR) + "/CuTest/CuAsm/InsAsmRepos/CuInsAsmRepos.sm_75.txt";
    CuAsm::CuInsAssemblerRepos repos(reposfile);

    std::string reposfile2 = std::string(CUASM_TESTDATA_DIR) + "/CuTest/CuInsAsmRepos.sm_75.txt";
    repos.merge(reposfile2);

    repos.completePredCodes();
    repos.save2file(std::string(CUASM_TESTDATA_DIR) + "/CuTest/new.sm_75.txt");

    return 0;
}
