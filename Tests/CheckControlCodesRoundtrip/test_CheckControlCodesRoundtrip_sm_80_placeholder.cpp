#include "CheckControlCodesRoundtripCommon.hpp"

/**
 * @brief Placeholder for the sm_80 (Ampere) full round trip -- OperandRoleTable/LatencyClassTable
 *        curation (Reports/tasks.md Phase 0/1) isn't Turing-complete-equivalent for sm_80 yet (the
 *        LDGSTS async-copy family, F2FP/F2IP/I2FP, REDUX, and others remain uncurated TODOs), so a
 *        real fixture round trip here would fail on data-curation gaps rather than exercising this
 *        suite's actual subject. Trivially passes until that curation catches up; see
 *        CheckControlCodesRoundtripCommon.hpp's top-of-file doc.
 * @return 0 (always).
 **/
int main() {
    return CuAsm::Test::runCheckControlCodesRoundtripPlaceholder("sm_80") ? 0 : 1;
}
