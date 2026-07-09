// Single-kernel fixture for the CheckDisasm round-trip tests: exercises a grid-wide cooperative
// groups barrier (this_grid().sync()), a synchronization instruction family distinct from the
// block-wide __syncthreads() covered by SequentialBarriers.
#include <cooperative_groups.h>

namespace cg = cooperative_groups;

extern "C" __global__ void CooperativeGroups(const float* in, float* out, int n)
{
    cg::grid_group grid = cg::this_grid();
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    float v = (idx < n) ? in[idx] : 0.0f;
    grid.sync();

    if (idx < n) {
        out[idx] = v * 2.0f;
    }
}
