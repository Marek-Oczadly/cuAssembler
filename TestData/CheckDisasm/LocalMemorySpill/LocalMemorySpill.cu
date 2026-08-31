// Single-kernel fixture for the CheckDisasm round-trip tests: exercises local-memory spill
// instructions (LDL/STL) by using a per-thread local array too large to stay entirely in
// registers, with unroll disabled so the array indexing isn't optimized away.
extern "C" __global__ void LocalMemorySpill(const float* in, float* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    float local[64];

#pragma unroll 1
    for (int i = 0; i < 64; ++i) {
        local[i] = (idx < n) ? in[(idx + i) % n] : 0.0f;
    }

    float acc = 0.0f;
#pragma unroll 1
    for (int i = 0; i < 64; ++i) {
        acc += local[i] * local[63 - i];
    }

    if (idx < n) {
        out[idx] = acc;
    }
}
