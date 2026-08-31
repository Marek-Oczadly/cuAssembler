// Single-kernel fixture for the CheckDisasm round-trip tests: exercises reads from __constant__
// memory (LDC instructions), not exercised by any of the global/shared-memory fixtures.
__constant__ float coeffs[16];

extern "C" __global__ void ConstantMemory(const float* in, float* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        out[idx] = in[idx] * coeffs[idx % 16] + coeffs[0];
    }
}
