// Single-kernel fixture for the CheckDisasm round-trip tests: exercises several sequential
// stages of shared-memory work separated by __syncthreads(), to check that barrier placement
// (and the ordering of the surrounding shared-memory accesses) survives disassembly and
// reassembly.
extern "C" __global__ void SequentialBarriers(const float* in, float* out, int n)
{
    __shared__ float stage[128];

    int tid = threadIdx.x;
    int idx = blockIdx.x * blockDim.x + tid;

    stage[tid] = (idx < n) ? in[idx] : 0.0f;
    __syncthreads();

    stage[tid] = stage[tid] * 2.0f;
    __syncthreads();

    float neighbor = (tid + 1 < blockDim.x) ? stage[tid + 1] : stage[tid];
    __syncthreads();

    stage[tid] = stage[tid] + neighbor;
    __syncthreads();

    if (idx < n) {
        out[idx] = stage[tid];
    }
}
