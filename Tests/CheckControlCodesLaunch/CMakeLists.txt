# CheckControlCodesLaunch: real-GPU-launch differential test (Reports/correct-cc-insKey-drops-
# width-modifier.md and its four predecessors). Every other CheckControlCodes*-family suite
# validates a correction against this codebase's *own* hazard model (verifyControlCodes()'s
# self-check) -- which cannot catch a bug shared between correctControlCodes() and
# verifyControlCodes() themselves. This suite instead compiles a real, scoreboard-slot-pressure
# kernel, corrects its control codes, and actually launches both the original and corrected cubins
# via the CUDA Driver API on this machine's GPU, comparing real output -- five bugs in a row this
# session were only ever caught this way, via a *different* project's training run; this brings
# that same check in-repo. Independently guarded on find_package(CUDAToolkit QUIET), same pattern
# as bin/cc-diff-run.cpp's own guard, since (unlike every other test here, which only shells out to
# nvcc as a subprocess) this one needs to link the CUDA Driver API library itself.
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    add_executable(test_CheckControlCodesLaunch_TiledGemm_sm_75 test_CheckControlCodesLaunch_TiledGemm_sm_75.cpp)
    target_link_libraries(test_CheckControlCodesLaunch_TiledGemm_sm_75 PRIVATE CuAsm elfio::elfio Boost::graph CUDA::cuda_driver)
    add_test(NAME test_CheckControlCodesLaunch_TiledGemm_sm_75 COMMAND test_CheckControlCodesLaunch_TiledGemm_sm_75)
else()
    message(STATUS "CUDAToolkit not found; skipping CheckControlCodesLaunch (needs the CUDA Driver API to launch kernels).")
endif()
