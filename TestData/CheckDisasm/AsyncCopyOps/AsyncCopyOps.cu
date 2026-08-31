// Single-kernel fixture for the CheckDisasm round-trip tests: exercises Ampere's asynchronous
// global-to-shared memory copy (cp.async, via __pipeline_memcpy_async/__pipeline_commit/
// __pipeline_wait_prior). Below sm_80, nvcc silently lowers these calls to an ordinary
// LDG+STS instead of erroring, which would defeat the point of this fixture, so the sm_80-and-
// newer requirement is enforced by minArchSM in CheckDisasmCommon.hpp rather than a compile
// guard here.
#include <cuda_pipeline.h>

extern "C" __global__ void AsyncCopyOps(const float* in, float* out, int n)
{
    extern __shared__ float tile[];

    int tid = threadIdx.x;
    int idx = blockIdx.x * blockDim.x + tid;

    __pipeline_memcpy_async(&tile[tid], &in[idx], sizeof(float));
    __pipeline_commit();
    __pipeline_wait_prior(0);
    __syncthreads();

    if (idx < n) {
        out[idx] = tile[tid] * 2.0f;
    }
}
