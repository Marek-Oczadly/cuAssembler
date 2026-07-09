#include "../CheckDisasmCommon.hpp"

/**
 * @brief Round-trips the TextureOps kernel through nvcc -> disassemble -> reassemble for
 *        sm_70 and checks that the reassembled cubin is byte-for-byte identical to the
 *        cubin nvcc originally produced.
 * @return 0 if the reassembled cubin matches the original, 1 otherwise.
 **/
int main() {
    return CuAsm::Test::runCheckDisasm("TextureOps", "sm_70") ? 0 : 1;
}
