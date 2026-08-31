#include "../CheckDisasmCommon.hpp"

/**
 * @brief Round-trips the TensorCoreOps kernel through nvcc -> disassemble -> reassemble for
 *        sm_75 and checks that the reassembled cubin is byte-for-byte identical to the
 *        cubin nvcc originally produced. Automatically passes without invoking nvcc if
 *        sm_75 is older than sm_70, since this kernel's instructions require
 *        sm_70 or newer.
 * @return 0 if the reassembled cubin matches the original (or the check was skipped), 1
 *         otherwise.
 **/
int main() {
    return CuAsm::Test::runCheckDisasm("TensorCoreOps", "sm_75", 70) ? 0 : 1;
}
