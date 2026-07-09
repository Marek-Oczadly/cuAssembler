// Single-kernel fixture for the CheckDisasm round-trip tests: exercises shared memory, both
// statically-sized and dynamically-sized (extern), with a block-wide power-of-two reduction
// over the static tile.
extern "C" __global__ void SharedMemory(const float* in, float* out, int n)
{
    __shared__ float partial[256];
    extern __shared__ float scratch[];

    int tid = threadIdx.x;
    int idx = blockIdx.x * blockDim.x + tid;

    partial[tid] = (idx < n) ? in[idx] : 0.0f;
    scratch[tid] = partial[tid] * 2.0f;
    __syncthreads();

    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (tid < stride) {
            partial[tid] += partial[tid + stride];
        }
        __syncthreads();
    }

    if (tid == 0) {
        out[blockIdx.x] = partial[0] + scratch[0];
    }
}
