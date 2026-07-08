#include "../CheckDisasmCommon.hpp"

/**
 * @brief Round-trips the AtomicOps kernel through nvcc -> disassemble -> reassemble for
 *        sm_75 and checks that the reassembled cubin is byte-for-byte identical to the
 *        cubin nvcc originally produced.
 * @return 0 if the reassembled cubin matches the original, 1 otherwise.
 **/
int main() {
    return CuAsm::Test::runCheckDisasm("AtomicOps", "sm_75") ? 0 : 1;
}
