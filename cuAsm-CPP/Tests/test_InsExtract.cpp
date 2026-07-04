#include <cstdio>
#include <iostream>
#include <string>

#include "../CuAsm/CuControlCode.hpp"
#include "../CuAsm/CuInsFeeder.hpp"
#include "../CuAsm/common.hpp"

/**
 * @brief Feeds instructions matching insfilter from fname and prints each as address, decoded
 *        control code, raw code, and asm, plus the code's binary/hex breakdown, mirroring the
 *        original doExtract().
 * @param fname Path of the dumped sass file to feed instructions from.
 * @param insfilter Regex filter string selecting particular instructions; empty means no filtering.
 * @param arch Arch string the sass belongs to.
 * @param maxcnt Maximum number of matching instructions to print.
 **/
void doExtract(const std::string& fname, const std::string& insfilter = "", const std::string& arch = "sm_75",
               int maxcnt = 100) {
    CuAsm::CuInsFeeder feeder(fname, arch, insfilter);

    std::cout << "# Searching " << fname << " with filter " << insfilter << ":" << std::endl;

    int cnt = 0;
    while (auto rec = feeder.next()) {
        std::string ctrlstr = CuAsm::CuControlCode::decode(rec->ctrl);
        std::printf("0x%03llx: [%s] %#016llx  %s \n",
                    static_cast<unsigned long long>(rec->addr), ctrlstr.c_str(),
                    static_cast<unsigned long long>(rec->code), rec->asmText.c_str());
        std::cout << CuAsm::binstr(rec->code, 64) << std::endl;
        std::cout << CuAsm::hexstr(rec->code, 64) << std::endl;

        cnt += 1;
        if (cnt >= maxcnt) {
            break;
        }
    }
}

/**
 * @brief Entry point mirroring the original Tests/test_InsExtract.py script.
 * @return 0 on success.
 **/
int main() {
    std::string sassname1 = R"(G:\Temp\cudnn64_7.sm_50.sass)";
    // std::string sassname2 = R"(G:\Temp\cudnn64_7.sass)";

    // initialize a feeder with sass
    doExtract(sassname1, R"(FADD.*0\.5)", "sm_50", 10);

    return 0;
}
