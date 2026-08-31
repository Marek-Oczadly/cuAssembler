#include "../CheckControlCodesRoundtripCommon.hpp"

/**
 * @brief Full compile -> disassemble -> shuffle -> validate/correct -> re-assemble -> reload ->
 *        re-verify round trip for the LargeKernel kernel fixture (sm_75) -- see
 *        CheckControlCodesRoundtripCommon.hpp's top-of-file doc for the full pipeline this covers.
 * @return 0 if every step succeeded, 1 otherwise.
 **/
int main() {
    return CuAsm::Test::runCheckControlCodesRoundtrip("LargeKernel", "sm_75") ? 0 : 1;
}
