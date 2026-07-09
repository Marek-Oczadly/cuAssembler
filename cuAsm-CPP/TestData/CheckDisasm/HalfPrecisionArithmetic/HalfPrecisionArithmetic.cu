// Single-kernel fixture for the CheckDisasm round-trip tests: exercises packed/scalar
// half-precision arithmetic instructions (__hadd, __hmul, __hfma) via the cuda_fp16 API,
// distinct from TensorCoreOps' WMMA fragment-based half-precision usage.
#include <cuda_fp16.h>

extern "C" __global__ void HalfPrecisionArithmetic(const __half* a, const __half* b, __half* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        __half x = a[idx];
        __half y = b[idx];

        __half s = __hadd(x, y);
        __half m = __hmul(x, y);
        __half f = __hfma(x, y, x);

        out[idx] = __hadd(s, __hadd(m, f));
    }
}
