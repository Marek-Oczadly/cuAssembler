#include "../CheckDisasmCommon.hpp"

/**
 * @brief Round-trips the MatrixMultiply kernel through nvcc -> disassemble -> reassemble for
 *        sm_60 and checks that the reassembled cubin is byte-for-byte identical to the
 *        cubin nvcc originally produced.
 * @return 0 if the reassembled cubin matches the original, 1 otherwise.
 **/
int main() {
    return CuAsm::Test::runCheckDisasm("MatrixMultiply", "sm_60") ? 0 : 1;
}
