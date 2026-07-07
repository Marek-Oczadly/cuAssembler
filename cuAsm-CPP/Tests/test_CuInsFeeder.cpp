#include <iomanip>
#include <iostream>
#include <string>

#include "../CuAsm/CuInsFeeder.hpp"
#include "../CuAsm/CuInsParser.hpp"
#include "../CuAsm/utils/BigNum.hpp"

/**
 * @brief Feeds and prints every instruction record from an sm_61 dumped sass file, mirroring the
 *        original test_sm61().
 **/
void test_sm61() {
    std::string fname = std::string(CUASM_TESTDATA_DIR) + "/CuTest/cudatest.sm_61.sass";
    CuAsm::CuInsFeeder feeder(fname, "sm_61");

    while (auto rec = feeder.next()) {
        std::cout << "0x" << std::hex << std::setfill('0') << std::setw(4) << rec->addr
                  << " :   0x" << std::setw(6) << rec->ctrl
                  << "   0x" << std::setw(16) << rec->code
                  << "   " << rec->asmText << std::dec << std::endl;
    }
}

/**
 * @brief Feeds instructions from an sm_75 dumped sass file, parses the first several with
 *        CuInsParser, and prints the record plus parsed key/values/modifiers, mirroring the
 *        original test_sm75().
 **/
void test_sm75() {
    int cnt = 0;
    std::string fname = std::string(CUASM_TESTDATA_DIR) + "/CuTest/cudatest.sm_75.sass";
    CuAsm::CuInsFeeder feeder(fname, "sm_75");

    CuAsm::CuInsParser cip("sm_75");

    while (auto rec = feeder.next()) {
        std::cout << "0x" << std::hex << std::setfill('0') << std::setw(4) << rec->addr
                  << " :   0x" << std::setw(6) << rec->ctrl
                  << "   0x" << std::setw(28) << rec->code
                  << "   " << rec->asmText << std::dec << std::endl;

        // CuInsParser::parse's code param is uint64_t (kept only for dumpInfo() debug display, not
        // used in parsing itself), so only the low 64 bits of the record's (possibly wider) code
        // are passed through here.
        const std::uint64_t codeLow64 = (rec->code & ((CuAsm::BigInt(1) << 64) - 1)).convert_to<std::uint64_t>();
        auto [insKey, insVals, insModi] = cip.parse(rec->asmText, rec->addr, codeLow64);

        std::cout << "    Ins_Key = " << insKey << std::endl;

        std::cout << "    Ins_Vals = [";
        for (std::size_t i = 0; i < insVals.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << insVals[i];
        }
        std::cout << "]" << std::endl;

        std::cout << "    Ins_Modi = [";
        for (std::size_t i = 0; i < insModi.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << insModi[i];
        }
        std::cout << "]" << std::endl;

        if (++cnt > 10) {
            break;
        }
    }
}

/**
 * @brief Entry point mirroring the original Tests/test_CuInsFeeder.py script.
 * @return 0 on success.
 **/
int main() {
    test_sm75();
    // test_sm61();

    return 0;
}
