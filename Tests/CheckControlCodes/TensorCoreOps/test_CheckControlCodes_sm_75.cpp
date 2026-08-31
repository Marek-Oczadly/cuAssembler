#include "../CheckControlCodesCommon.hpp"

/**
 * @brief Compiles the TensorCoreOps kernel fixture for sm_75, decodes its real ptxas-scheduled control
 *        codes, and runs hazard-based verification against every kernel function it produces --
 *        the Reports/tasks.md Phase 5 fixture-content check for OperandRoleTable/LatencyClassTable.
 * @return 0 if every kernel function decodes and verifies clean, 1 otherwise.
 **/
int main() {
    return CuAsm::Test::runCheckControlCodes("TensorCoreOps", "sm_75", 70) ? 0 : 1;
}
