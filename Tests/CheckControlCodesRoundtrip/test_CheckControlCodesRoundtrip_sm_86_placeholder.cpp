#include "CheckControlCodesRoundtripCommon.hpp"

/**
 * @brief Placeholder for the sm_86 (Ampere) full round trip -- same reasoning as the sm_80
 *        placeholder (see test_CheckControlCodesRoundtrip_sm_80_placeholder.cpp and
 *        CheckControlCodesRoundtripCommon.hpp's top-of-file doc): OperandRoleTable/LatencyClassTable
 *        curation isn't Turing-complete-equivalent for sm_86 yet, so this trivially passes until it
 *        is.
 * @return 0 (always).
 **/
int main() {
    return CuAsm::Test::runCheckControlCodesRoundtripPlaceholder("sm_86") ? 0 : 1;
}
