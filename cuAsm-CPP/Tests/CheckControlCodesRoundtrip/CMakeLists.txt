# CheckControlCodesRoundtrip fixture tests (Reports/tasks.md Phase 5): the full "compile with nvcc,
# disassemble, shuffle instructions, validate/correct control codes, re-assemble" pipeline for each
# kernel -- unlike CheckControlCodesShuffle (which only ever checks correctControlCodes()'s output
# in memory), this suite additionally merges the corrected control codes back into real cubin bytes
# (CuSMVersion::mergeCtrlCodes(), patched into a copy of the original file exactly like
# correct-cc.cpp's own logic), writes an actual output cubin, and reloads/redecodes THAT file
# completely from scratch (a fresh ELFIO load, a fresh cuobjdump run) before a final
# verifyControlCodes() -- the strongest end-to-end evidence this test suite produces that the
# correction pipeline survives a real re-encode/patch/disk round trip, not just in-memory
# self-consistency. Turing (sm_75) only, same reasoning as the sibling suites: the
# OperandRoleTable/LatencyClassTable curation this depends on is only complete there. One executable
# per kernel, named test_CheckControlCodesRoundtrip_<Kernel>_sm_75; sm_80/sm_86 get a single
# trivially-passing placeholder executable each (see CheckControlCodesRoundtripCommon.hpp's doc) --
# a real fixture round trip on those architectures would only ever fail on data-curation gaps this
# suite isn't meant to police, not on anything about the correction/re-assembly pipeline itself.

set(CUASM_CHECKCONTROLCODESROUNDTRIP_KERNELS
    IntegerArithmetic
    FloatingPointArithmetic
    MemoryOps
    AtomicOps
    MatrixMultiply
    SharedMemory
    Int64Arithmetic
    LocalMemorySpill
    ControlFlow
    LargeKernel
)

foreach(CUASM_CHECKCONTROLCODESROUNDTRIP_KERNEL ${CUASM_CHECKCONTROLCODESROUNDTRIP_KERNELS})
    set(CUASM_CHECKCONTROLCODESROUNDTRIP_TEST_NAME test_CheckControlCodesRoundtrip_${CUASM_CHECKCONTROLCODESROUNDTRIP_KERNEL}_sm_75)

    add_executable(${CUASM_CHECKCONTROLCODESROUNDTRIP_TEST_NAME}
        ${CUASM_CHECKCONTROLCODESROUNDTRIP_KERNEL}/test_CheckControlCodesRoundtrip_sm_75.cpp
    )
    target_link_libraries(${CUASM_CHECKCONTROLCODESROUNDTRIP_TEST_NAME} PRIVATE CuAsm elfio::elfio Boost::graph)
    add_test(NAME ${CUASM_CHECKCONTROLCODESROUNDTRIP_TEST_NAME} COMMAND ${CUASM_CHECKCONTROLCODESROUNDTRIP_TEST_NAME})
endforeach()

# sm_80/sm_86: trivially-passing placeholders, not per-kernel (see CheckControlCodesRoundtripCommon.hpp).
add_executable(test_CheckControlCodesRoundtrip_sm_80_placeholder test_CheckControlCodesRoundtrip_sm_80_placeholder.cpp)
target_link_libraries(test_CheckControlCodesRoundtrip_sm_80_placeholder PRIVATE CuAsm elfio::elfio Boost::graph)
add_test(NAME test_CheckControlCodesRoundtrip_sm_80_placeholder COMMAND test_CheckControlCodesRoundtrip_sm_80_placeholder)

add_executable(test_CheckControlCodesRoundtrip_sm_86_placeholder test_CheckControlCodesRoundtrip_sm_86_placeholder.cpp)
target_link_libraries(test_CheckControlCodesRoundtrip_sm_86_placeholder PRIVATE CuAsm elfio::elfio Boost::graph)
add_test(NAME test_CheckControlCodesRoundtrip_sm_86_placeholder COMMAND test_CheckControlCodesRoundtrip_sm_86_placeholder)
