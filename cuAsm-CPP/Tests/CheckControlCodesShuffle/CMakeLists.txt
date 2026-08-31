# CheckControlCodesShuffle fixture tests (Reports/tasks.md Phase 5): for each kernel, nvcc compiles
# the same .cu fixture CheckDisasm/CheckControlCodes already use (TestData/CheckDisasm/<Kernel>) to
# a fresh cubin, cuAsm-CPP decodes its real ptxas-scheduled control codes, physically shuffles a
# legal subset of its instructions into a genuinely different (dependency-respecting) order, and
# checks that correctControlCodes() repairs the result into control codes that independently
# re-verify clean -- unlike CheckControlCodes, which only ever checks a kernel's original,
# unmodified instruction order. Turing (sm_75) only, same reasoning as CheckControlCodes: the
# OperandRoleTable/LatencyClassTable curation this depends on is only complete there. One executable
# per kernel, named test_CheckControlCodesShuffle_<Kernel>_sm_75.

set(CUASM_CHECKCONTROLCODESSHUFFLE_KERNELS
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

foreach(CUASM_CHECKCONTROLCODESSHUFFLE_KERNEL ${CUASM_CHECKCONTROLCODESSHUFFLE_KERNELS})
    set(CUASM_CHECKCONTROLCODESSHUFFLE_TEST_NAME test_CheckControlCodesShuffle_${CUASM_CHECKCONTROLCODESSHUFFLE_KERNEL}_sm_75)

    add_executable(${CUASM_CHECKCONTROLCODESSHUFFLE_TEST_NAME}
        ${CUASM_CHECKCONTROLCODESSHUFFLE_KERNEL}/test_CheckControlCodesShuffle_sm_75.cpp
    )
    target_link_libraries(${CUASM_CHECKCONTROLCODESSHUFFLE_TEST_NAME} PRIVATE CuAsm elfio::elfio Boost::graph)
    add_test(NAME ${CUASM_CHECKCONTROLCODESSHUFFLE_TEST_NAME} COMMAND ${CUASM_CHECKCONTROLCODESSHUFFLE_TEST_NAME})
endforeach()
