#include "../CheckDisasmCommon.hpp"

/**
 * @brief Round-trips the AsyncCopyOps kernel through nvcc -> disassemble -> reassemble for
 *        sm_75 and checks that the reassembled cubin is byte-for-byte identical to the
 *        cubin nvcc originally produced. Automatically passes without invoking nvcc if
 *        sm_75 is older than sm_80, since this kernel's instructions require
 *        sm_80 or newer.
 * @return 0 if the reassembled cubin matches the original (or the check was skipped), 1
 *         otherwise.
 **/
int main() {
    return CuAsm::Test::runCheckDisasm("AsyncCopyOps", "sm_75", 80) ? 0 : 1;
}
