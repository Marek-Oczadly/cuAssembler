// Single-kernel fixture for the CheckDisasm round-trip tests: exercises global and dynamic
// shared memory access (loads/stores plus a shared-memory neighbor-sum pattern).
extern "C" __global__ void MemoryOps(const float* in, float* out, int n)
{
    extern __shared__ float tile[];

    int tid = threadIdx.x;
    int idx = blockIdx.x * blockDim.x + tid;

    tile[tid] = (idx < n) ? in[idx] : 0.0f;
    __syncthreads();

    float neighborSum = tile[tid];
    if (tid > 0) {
        neighborSum += tile[tid - 1];
    }
    if (tid < blockDim.x - 1) {
        neighborSum += tile[tid + 1];
    }

    if (idx < n) {
        out[idx] = neighborSum;
    }
}
