// Single-kernel fixture for the CheckDisasm round-trip tests: exercises atomic instructions
// (atomicAdd, atomicMax, atomicCAS).
extern "C" __global__ void AtomicOps(int* counters, int* maxVal, int* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        int old = atomicAdd(&counters[idx % 32], 1);
        atomicMax(maxVal, idx);
        atomicCAS(&counters[(idx + 1) % 32], old, old + 1);
        out[idx] = old;
    }
}
