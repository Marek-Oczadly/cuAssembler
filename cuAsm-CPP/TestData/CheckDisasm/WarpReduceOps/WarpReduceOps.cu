// Single-kernel fixture for the CheckDisasm round-trip tests: exercises warp-level vote/match/
// reduce instructions not covered by Intrinsics (__all_sync, __any_sync, __match_any_sync, and
// Ampere's __reduce_add_sync). __reduce_add_sync requires sm_80 (Ampere) or newer, so the whole
// kernel is gated on sm_80 via minArchSM in CheckDisasmCommon.hpp, matching the AsyncCopyOps/
// TensorCoreOps pattern, rather than macro-guarding just the reduce call.
extern "C" __global__ void WarpReduceOps(const int* in, int* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    int v = (idx < n) ? in[idx] : 0;

    bool allPos = __all_sync(0xffffffffu, v > 0);
    bool anyNeg = __any_sync(0xffffffffu, v < 0);
    unsigned matchMask = __match_any_sync(0xffffffffu, v % 4);
    int reduced = __reduce_add_sync(0xffffffffu, v);

    if (idx < n) {
        out[idx] = v + static_cast<int>(allPos) + static_cast<int>(anyNeg)
                 + static_cast<int>(matchMask) + reduced;
    }
}
