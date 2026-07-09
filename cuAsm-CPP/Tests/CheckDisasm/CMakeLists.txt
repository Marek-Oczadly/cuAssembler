# CheckDisasm round-trip tests: for each (kernel, architecture) pair, nvcc compiles the
# kernel's .cu fixture under TestData/CheckDisasm/<Kernel> to a .cubin, cuAsm-CPP disassembles
# it, reassembles that disassembly, and the result is compared byte-for-byte against the
# original cubin. One executable per pair, named test_CheckDisasm_<Kernel>_<Arch>.

set(CUASM_CHECKDISASM_KERNELS
    IntegerArithmetic
    FloatingPointArithmetic
    MemoryOps
    ControlFlow
    AtomicOps
    TensorCoreOps
    Intrinsics
    LargeKernel
    AsyncCopyOps
)

set(CUASM_CHECKDISASM_ARCHS
    sm_60
    sm_61
    sm_70
    sm_75
    sm_80
    sm_86
)

foreach(CUASM_CHECKDISASM_KERNEL ${CUASM_CHECKDISASM_KERNELS})
    foreach(CUASM_CHECKDISASM_ARCH ${CUASM_CHECKDISASM_ARCHS})
        set(CUASM_CHECKDISASM_TEST_NAME test_CheckDisasm_${CUASM_CHECKDISASM_KERNEL}_${CUASM_CHECKDISASM_ARCH})

        add_executable(${CUASM_CHECKDISASM_TEST_NAME}
            ${CUASM_CHECKDISASM_KERNEL}/test_CheckDisasm_${CUASM_CHECKDISASM_ARCH}.cpp
        )
        target_link_libraries(${CUASM_CHECKDISASM_TEST_NAME} PRIVATE CuAsm)
        add_test(NAME ${CUASM_CHECKDISASM_TEST_NAME} COMMAND ${CUASM_CHECKDISASM_TEST_NAME})
    endforeach()
endforeach()
