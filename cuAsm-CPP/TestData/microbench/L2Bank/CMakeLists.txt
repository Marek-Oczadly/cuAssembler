# --- Host-side C++ tool (test_resplot.py port) --------------------------------------------

add_executable(l2bank_resplot test_resplot.cpp)

# --- CUDA microbenchmark (l2bank_test.cu / makefile port) ----------------------------------
#
# Mirrors the legacy makefile's nvcc build plus its cuobjdump-based dsass/dcubin/dptx targets.
# Only enabled if a CUDA compiler is actually available, so machines without the CUDA toolchain
# can still configure/build the rest of the project.

include(CheckLanguage)
check_language(CUDA)

if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)

    add_executable(l2bank_test l2bank_test.cu)
    # Pinned independently of the host project's CMAKE_CXX_STANDARD (currently 23): nvcc's
    # MSVC host-compiler pass only supports up to C++20, so this must not follow the host bump.
    set_target_properties(l2bank_test PROPERTIES
        CUDA_ARCHITECTURES "50;75"
        CUDA_STANDARD 20
        CUDA_STANDARD_REQUIRED ON
    )

    # Matches the makefile's INCLUDE/LINK variables verbatim (hardcoded to the original dev
    # machine's Windows SDK install); adjust if building on a different machine.
    target_include_directories(l2bank_test PRIVATE
        "C:/Program Files (x86)/Windows Kits/10/Include/10.0.18362.0/ucrt"
    )
    target_link_libraries(l2bank_test PRIVATE TestDataInclude)
    target_link_directories(l2bank_test PRIVATE
        "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.18362.0/ucrt/x64"
        "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.18362.0/um/x64"
    )

    find_program(CUOBJDUMP_EXECUTABLE cuobjdump)
    if(CUOBJDUMP_EXECUTABLE)
        # make dsass
        add_custom_target(l2bank_dsass
            COMMAND ${CUOBJDUMP_EXECUTABLE} -sass -arch sm_75 $<TARGET_FILE:l2bank_test> > l2bank_test.sm_75.sass
            COMMAND ${CUOBJDUMP_EXECUTABLE} -sass -arch sm_50 $<TARGET_FILE:l2bank_test> > l2bank_test.sm_50.sass
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS l2bank_test
            VERBATIM
        )

        # make dcubin
        add_custom_target(l2bank_dcubin
            COMMAND ${CUOBJDUMP_EXECUTABLE} -xelf all $<TARGET_FILE:l2bank_test>
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS l2bank_test
            VERBATIM
        )

        # make dptx
        add_custom_target(l2bank_dptx
            COMMAND ${CUOBJDUMP_EXECUTABLE} -ptx $<TARGET_FILE:l2bank_test> > l2bank_test.ptx
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS l2bank_test
            VERBATIM
        )
    else()
        message(STATUS "cuobjdump not found; skipping dsass/dcubin/dptx targets for l2bank_test.")
    endif()

    # `make clean`'s ${BASENAME}.exe removal is superseded by CMake's own built-in clean target.
else()
    message(STATUS "No CUDA compiler found; skipping l2bank_test CUDA build (TestData/microbench/L2Bank).")
endif()
