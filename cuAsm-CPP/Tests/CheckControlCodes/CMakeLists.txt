# CheckControlCodes fixture tests (Reports/tasks.md Phase 5): for each kernel, nvcc compiles the
# same .cu fixture CheckDisasm already uses (TestData/CheckDisasm/<Kernel>) to a fresh cubin,
# cuAsm-CPP decodes its real ptxas-scheduled control codes (bin/ccCommon.hpp's decodeInstructions()),
# and runs hazard-based verification (verifyControlCodes()) against every kernel function found.
# Turing (sm_75) only, on purpose -- Reports/tasks.md's OperandRoleTable/LatencyClassTable are fully
# curated (zero TODOs) for sm_75, but sm_80/sm_86 still carry real TODO gaps, so running this same
# check there would fail on data-curation gaps this test isn't meant to police. One executable per
# kernel, named test_CheckControlCodes_<Kernel>_sm_75.

set(CUASM_CHECKCONTROLCODES_KERNELS
    IntegerArithmetic
    FloatingPointArithmetic
    MemoryOps
    ControlFlow
    AtomicOps
    TensorCoreOps
    Intrinsics
    LargeKernel
    DeviceFunctionCalls
    SharedMemory
    SequentialBarriers
    InlinePTX
    MatrixMultiply
    DoublePrecisionArithmetic
    HalfPrecisionArithmetic
    TextureOps
    ConstantMemory
    Int64Arithmetic
    ExtendedAtomicOps
    LocalMemorySpill
    DevicePrintf
    CacheHintOps
    CooperativeGroups
)

foreach(CUASM_CHECKCONTROLCODES_KERNEL ${CUASM_CHECKCONTROLCODES_KERNELS})
    set(CUASM_CHECKCONTROLCODES_TEST_NAME test_CheckControlCodes_${CUASM_CHECKCONTROLCODES_KERNEL}_sm_75)

    add_executable(${CUASM_CHECKCONTROLCODES_TEST_NAME}
        ${CUASM_CHECKCONTROLCODES_KERNEL}/test_CheckControlCodes_sm_75.cpp
    )
    target_link_libraries(${CUASM_CHECKCONTROLCODES_TEST_NAME} PRIVATE CuAsm elfio::elfio Boost::graph)
    add_test(NAME ${CUASM_CHECKCONTROLCODES_TEST_NAME} COMMAND ${CUASM_CHECKCONTROLCODES_TEST_NAME})
endforeach()
