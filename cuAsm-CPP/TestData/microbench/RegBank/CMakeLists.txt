# --- Host-side C++ tools (test_bc_enum.py / test_regbank.py ports) -------------------------

# Shared bank-conflict enumeration logic (buildCombDict/getCombStr/genBankConflictInsSeq),
# used by both test_bc_enum's own CLI and test_regbank's doTest_BankConflictComb, mirroring
# Python's `from test_bc_enum import ...`.
add_library(RegBankBcEnum STATIC test_bc_enum_core.cpp)
target_include_directories(RegBankBcEnum PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_bc_enum test_bc_enum.cpp)
target_link_libraries(test_bc_enum PRIVATE RegBankBcEnum)

add_executable(test_regbank test_regbank.cpp)
target_link_libraries(test_regbank PRIVATE RegBankBcEnum CuAsm)

# --- CUDA microbenchmark (regbank_test.cu / makefile port) ----------------------------------
#
# Mirrors the legacy makefile's nvcc build plus its cuobjdump-based dsass/dcubin/dptx targets.
# Only enabled if a CUDA compiler is actually available, so machines without the CUDA toolchain
# can still configure/build the rest of the project.

include(CheckLanguage)
check_language(CUDA)

if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)

    add_executable(regbank_test regbank_test.cu)
    set_target_properties(regbank_test PROPERTIES CUDA_ARCHITECTURES 50)

    # Matches the makefile's INCLUDE/LINK variables verbatim (hardcoded to the original dev
    # machine's Windows SDK install); adjust if building on a different machine.
    target_include_directories(regbank_test PRIVATE
        "C:/Program Files (x86)/Windows Kits/10/Include/10.0.18362.0/ucrt"
    )
    target_link_libraries(regbank_test PRIVATE TestDataInclude)
    target_link_directories(regbank_test PRIVATE
        "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.18362.0/ucrt/x64"
        "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.18362.0/um/x64"
    )

    # regbank_test.cu uses the CUDA Driver API (cuInit/cuModuleLoad/cuLaunchKernel) to load the
    # cuasm-assembled .cubin at runtime; the makefile's -L dirs alone don't provide cuda.lib.
    find_package(CUDAToolkit REQUIRED)
    target_link_libraries(regbank_test PRIVATE CUDA::cuda_driver)

    find_program(CUOBJDUMP_EXECUTABLE cuobjdump)
    if(CUOBJDUMP_EXECUTABLE)
        # make dsass
        # (named regbank_dsass/dcubin/dptx to avoid clashing with bin/dsass, the CuAsm CLI tool)
        add_custom_target(regbank_dsass
            COMMAND ${CUOBJDUMP_EXECUTABLE} -sass -arch sm_75 $<TARGET_FILE:regbank_test> > regbank_test.sm_75.sass
            COMMAND ${CUOBJDUMP_EXECUTABLE} -sass -arch sm_50 $<TARGET_FILE:regbank_test> > regbank_test.sm_50.sass
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS regbank_test
            VERBATIM
        )

        # make dcubin
        add_custom_target(regbank_dcubin
            COMMAND ${CUOBJDUMP_EXECUTABLE} -xelf all $<TARGET_FILE:regbank_test>
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS regbank_test
            VERBATIM
        )

        # make dptx
        add_custom_target(regbank_dptx
            COMMAND ${CUOBJDUMP_EXECUTABLE} -xptx all $<TARGET_FILE:regbank_test>
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS regbank_test
            VERBATIM
        )
    else()
        message(STATUS "cuobjdump not found; skipping dsass/dcubin/dptx targets for regbank_test.")
    endif()

    # `make clean`'s ${BASENAME}.exe removal is superseded by CMake's own built-in clean target.
else()
    message(STATUS "No CUDA compiler found; skipping regbank_test CUDA build (TestData/microbench/RegBank).")
endif()
