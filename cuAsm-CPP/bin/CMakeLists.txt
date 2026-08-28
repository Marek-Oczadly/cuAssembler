# Target is named cuasm_cli (not cuasm) to avoid clashing with the CuAsm library target: VS
# solution/project names are case-insensitive, so "cuasm" and "CuAsm" collide there even though
# CMake itself treats them as distinct targets.
add_executable(cuasm_cli cuasm.cpp)
target_link_libraries(cuasm_cli PRIVATE CuAsm CLI11::CLI11)
set_target_properties(cuasm_cli PROPERTIES OUTPUT_NAME cuasm)

add_executable(dsass dsass.cpp)
target_link_libraries(dsass PRIVATE CuAsm CLI11::CLI11)

add_executable(hcubin hcubin.cpp)
target_link_libraries(hcubin PRIVATE CuAsm CLI11::CLI11)

add_executable(hnvcc hnvcc.cpp)
target_link_libraries(hnvcc PRIVATE CuAsm)

# verify-cc/correct-cc: CLI tools for control-code verification/correction. verify-cc's hazard
# analysis (ccCommon.hpp's verifyControlCodes(), Reports/tasks.md Phase 3) is implemented;
# correct-cc's (correctControlCodes(), Phase 4) is not yet -- see Reports/control-codes-validation.md.
# Both link Boost::graph since ccCommon.hpp's hazard graph (HazardGraph, a
# boost::adjacency_list) is pulled into any translation unit that includes it, even correct-cc.cpp
# which doesn't call into the hazard code itself yet.
add_executable(verify-cc verify-cc.cpp)
target_link_libraries(verify-cc PRIVATE CuAsm CLI11::CLI11 elfio::elfio Boost::graph)

add_executable(correct-cc correct-cc.cpp)
target_link_libraries(correct-cc PRIVATE CuAsm CLI11::CLI11 elfio::elfio Boost::graph)

# cc-diff-run: generic CUDA Driver-API host launcher for control-code correction differential
# testing (Reports/tasks.md Phase 5's compute-sanitizer checklist item). Needs the CUDA Driver API
# (cuda.h / cuda.lib, CUDA::cuda_driver via CMake's CUDAToolkit package -- distinct from the
# nvcc-as-a-language setup TestData/microbench/{L2Bank,RegBank} use) rather than just the nvcc
# compiler CheckDisasm's tests already require, so it's independently guarded and skipped (with a
# message, not a hard error) on a machine that has nvcc but not a full CUDA Toolkit install with
# the Driver API library.
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    add_executable(cc-diff-run cc-diff-run.cpp)
    target_link_libraries(cc-diff-run PRIVATE CLI11::CLI11 CUDA::cuda_driver)
else()
    message(STATUS "CUDAToolkit not found; skipping cc-diff-run (bin/cc-diff-run.cpp).")
endif()
