// Single-kernel fixture for the CheckDisasm round-trip tests: a larger kernel combining a wide
// variety of operations in one function (integer/float arithmetic, shared-memory neighbor-sum,
// a bounded loop, warp vote/shuffle, and atomics) to exercise round-tripping of a more realistic,
// mixed-instruction kernel body rather than one narrow instruction family at a time.
extern "C" __global__ void LargeKernel(const float* a, const float* b, const int* idx_in,
                                        float* out, int* histogram, int n)
{
    extern __shared__ float tile[];

    int tid = threadIdx.x;
    int idx = blockIdx.x * blockDim.x + tid;

    if (idx >= n) {
        return;
    }

    float x = a[idx];
    float y = b[idx];
    int   k = idx_in[idx];

    int sum  = k + tid;
    int diff = k - tid;
    int prod = k * tid;
    int bits = ((k << 2) ^ (tid >> 1)) | (k & tid);

    float fma  = fmaf(x, y, x);
    float divv = x / (y + 1.0f);
    float root = sqrtf(fabsf(x)) + rsqrtf(fabsf(y) + 1.0f);

    tile[tid] = fma + divv + root;
    __syncthreads();

    float neighborSum = tile[tid];
    if (tid > 0) {
        neighborSum += tile[tid - 1];
    }
    if (tid < blockDim.x - 1) {
        neighborSum += tile[tid + 1];
    }

    int acc = sum + diff + prod + bits;
    for (int i = 0; i < 4; ++i) {
        if (acc % 2 == 0) {
            acc += i;
        } else {
            acc -= i;
        }
        acc = acc >> 1;
        if (acc == 0) {
            break;
        }
    }

    unsigned mask  = __ballot_sync(0xffffffffu, acc > 0);
    int      shflv = __shfl_sync(0xffffffffu, acc, 0);

    atomicAdd(&histogram[tid % 32], 1);
    atomicMax(&histogram[31], acc);

    out[idx] = neighborSum + static_cast<float>(acc + shflv + static_cast<int>(mask));
}
