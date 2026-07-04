#include <string>

#include "../CuAsm/CuInsAssemblerRepos.hpp"

/**
 * @brief Loads an sm_75 instruction-assembler repos, merges in a second repos file, completes
 *        predicate codes, and saves the merged result, mirroring the original
 *        Tests/test_CuInsAsmRepos2.py script.
 * @return 0 on success.
 **/
int main() {
    std::string sassname = R"(G:\Repos\Tests\Programs\cudatest.sm_75.sass)";
    // std::string sassname = R"(G:\Temp\cudnn64_7.sm_50.sass)";
    std::string reposfile = R"(G:\Repos\CuAsm\InsAsmRepos\CuInsAsmRepos.sm_75.txt)";
    CuAsm::CuInsAssemblerRepos repos(reposfile);

    std::string reposfile2 = R"(G:\Repos\CuInsAsmRepos.sm_75.txt)";
    repos.merge(reposfile2);

    repos.completePredCodes();
    repos.save2file(R"(G:\Repos\new.sm_75.txt)");

    return 0;
}
