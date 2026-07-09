#include "../CheckDisasmCommon.hpp"

/**
 * @brief Round-trips the Int64Arithmetic kernel through nvcc -> disassemble -> reassemble for
 *        sm_61 and checks that the reassembled cubin is byte-for-byte identical to the
 *        cubin nvcc originally produced.
 * @return 0 if the reassembled cubin matches the original, 1 otherwise.
 **/
int main() {
    return CuAsm::Test::runCheckDisasm("Int64Arithmetic", "sm_61") ? 0 : 1;
}
