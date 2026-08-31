// Single-kernel fixture for the CheckDisasm round-trip tests: exercises common CUDA intrinsics
// (__syncthreads, __syncwarp, __ballot_sync, __shfl_sync, __shfl_down_sync, __activemask).
// __syncwarp requires sm_70 (Volta) or newer for meaningful independent-thread-scheduling
// semantics, so it's macro-guarded and simply omitted on older architectures rather than
// skipping the whole kernel; the warp-vote/shuffle intrinsics are available on every
// architecture this project targets (sm_60+).
extern "C" __global__ void Intrinsics(const int* in, int* out, int n)
{
    __shared__ int tile[32];

    int tid = threadIdx.x;
    int idx = blockIdx.x * blockDim.x + tid;

    int v = (idx < n) ? in[idx] : 0;
    tile[tid % 32] = v;
    __syncthreads();

#if __CUDA_ARCH__ >= 700
    __syncwarp();
#endif

    unsigned mask = __ballot_sync(0xffffffffu, v > 0);
    int shfl = __shfl_sync(0xffffffffu, v, 0);
    int down = __shfl_down_sync(0xffffffffu, v, 1);
    unsigned active = __activemask();

    int result = tile[tid % 32] + shfl + down + static_cast<int>(mask) + static_cast<int>(active);

    if (idx < n) {
        out[idx] = result;
    }
}
