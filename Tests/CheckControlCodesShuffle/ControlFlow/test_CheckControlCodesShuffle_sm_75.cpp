#include "../CheckControlCodesShuffleCommon.hpp"

/**
 * @brief Shuffles a legal subset of the ControlFlow kernel fixture's instructions (sm_75), runs
 *        correctControlCodes() against the reordered stream, and independently re-verifies the
 *        result -- the Reports/tasks.md Phase 5 "shuffling and moving instructions" check.
 * @return 0 if every kernel function's shuffle was handled correctly, 1 otherwise.
 **/
int main() {
    return CuAsm::Test::runCheckControlCodesShuffle("ControlFlow", "sm_75") ? 0 : 1;
}
