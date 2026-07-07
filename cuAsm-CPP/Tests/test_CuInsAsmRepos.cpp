#include <iostream>
#include <optional>
#include <string>

#include "../CuAsm/CuInsAssemblerRepos.hpp"
#include "../CuAsm/CuInsFeeder.hpp"

/**
 * @brief Builds an instruction-assembler repos from a dumped sass file, verifies it against the
 *        same sass, and optionally saves it.
 * @param sassname Path of the dumped sass file to feed instructions from.
 * @param savname Optional path to save the constructed repos to; unsaved if not given.
 * @param arch Arch string the sass belongs to.
 * @return The constructed repos.
 **/
CuAsm::CuInsAssemblerRepos constructReposFromFile(const std::string& sassname,
                                                   std::optional<std::string> savname = std::nullopt,
                                                   const std::string& arch = "sm_75") {
    // initialize a feeder with sass
    CuAsm::CuInsFeeder feeder(sassname, arch);

    // initialize an empty repos
    CuAsm::CuInsAssemblerRepos repos(std::nullopt, arch);

    // Update the repos with instructions from feeder
    repos.update(feeder);

    // reset the feeder back to start
    feeder.restart();

    // verify the repos
    // actually the codes is already verifed during repos construction
    repos.verify(feeder);

    if (savname.has_value()) {
        repos.save2file(savname.value());
    }

    return repos;
}

/**
 * @brief Verifies a previously saved instruction-assembler repos against a dumped sass file.
 * @param sassname Path of the dumped sass file to feed instructions from.
 * @param reposfile Path of the previously saved repos file to verify.
 * @param arch Arch string the sass/repos belong to.
 **/
void verifyReposFromFile(const std::string& sassname, const std::string& reposfile, const std::string& arch = "sm_75") {
    // initialize a feeder with sass
    CuAsm::CuInsFeeder feeder(sassname, arch);

    // initialize an empty repos
    CuAsm::CuInsAssemblerRepos repos(reposfile, arch);

    // verify the repos
    repos.verify(feeder);
}

/**
 * @brief Constructs and verifies an sm_86 instruction-assembler repos from a dumped sass file,
 *        mirroring the original Tests/test_CuInsAsmRepos.py script.
 * @return 0 on success.
 **/
int main() {
    std::string sassname = std::string(CUASM_TESTDATA_DIR) + "/CuTest/cudatest.sm_86.sass";
    // std::string sassname = std::string(CUASM_TESTDATA_DIR) + "/CuTest/cudnn64_7.sm_50.sass";
    std::string reposfile = std::string(CUASM_TESTDATA_DIR) + "/CuTest/CuInsAsmRepos.sm_86.txt";

    std::string arch = "sm_86";

    constructReposFromFile(sassname, reposfile, arch);
    std::cout << "### Construction done!" << std::endl;

    verifyReposFromFile(sassname, reposfile, arch);
    std::cout << "### Verification done!" << std::endl;

    return 0;
}
