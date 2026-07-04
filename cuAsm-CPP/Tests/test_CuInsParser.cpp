#include <iostream>
#include <string>

#include "../CuAsm/CuInsParser.hpp"

/**
 * @brief Entry point mirroring the original Tests/test_CuInsParser.py script: parses a single
 *        FFMA instruction and prints the parsed result and the parser itself.
 * @return 0 on success.
 **/
int main() {
    CuAsm::CuInsParser cip("sm_61");
    auto [insKey, insVals, insModi] = cip.parse("FFMA R9, R3.reuse, -0.5, R2.reuse ;");

    std::cout << "(" << insKey << ", [";
    for (std::size_t i = 0; i < insVals.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << insVals[i];
    }
    std::cout << "], [";
    for (std::size_t i = 0; i < insModi.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << insModi[i];
    }
    std::cout << "])" << std::endl;

    std::cout << &cip << std::endl;

    return 0;
}
