#include "CheckControlCodesLaunchCommon.hpp"

/**
 * @brief Compiles the TiledGemm kernel fixture for sm_75, corrects its control codes with no
 *        reorder applied, and actually launches both the original and corrected cubins on this
 *        machine's GPU, comparing real output -- the real-hardware analog of every
 *        CheckControlCodes-family / verify-cc self-check, and the check that would have caught all
 *        five Reports/correct-cc-....md bugs directly inside this repo instead of via SASS-Shuffler.
 * @return 0 if correction, harness sanity, and original-vs-corrected output all agree; 1 otherwise.
 **/
int main() {
    // 256x256 (16x16=256 thread blocks), matching SASS-Shuffler's example/gemm/ exactly: a wrong
    // scoreboard wait is a timing/occupancy-dependent race, not a deterministic logic error, so a
    // small grid (few concurrently-resident blocks) can fail to expose it even when the corrected
    // control codes are genuinely wrong -- confirmed directly this session, where the same
    // corrected cubin passed at 64x64 but the real training harness's 256x256 run hit real,
    // reproducible wrong output. Don't shrink this size without re-confirming it still reproduces.
    return CuAsm::Test::runCheckControlCodesLaunch("TiledGemm", "sm_75", 256) ? 0 : 1;
}
