// Single-kernel fixture for the CheckDisasm round-trip tests: exercises device-side printf,
// which uses a variadic vprintf calling convention distinct from the fixed-signature
// __device__ function calls covered by DeviceFunctionCalls.
#include <cstdio>

extern "C" __global__ void DevicePrintf(const int* in, int* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        int v = in[idx];
        printf("DevicePrintf: idx=%d value=%d\n", idx, v);
        out[idx] = v;
    }
}
