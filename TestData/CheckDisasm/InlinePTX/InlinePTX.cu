// Single-kernel fixture for the CheckDisasm round-trip tests: exercises inline PTX
// (asm volatile) mixed with ordinary C++ code, covering integer and floating-point inline
// asm plus a special-register read.
extern "C" __global__ void InlinePTX(const int* in, const float* fin, int* out, float* fout, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        int a = in[idx];
        int sum;
        asm volatile("add.s32 %0, %1, %2;" : "=r"(sum) : "r"(a), "r"(idx));

        float fa = fin[idx];
        float fsum;
        asm volatile("add.f32 %0, %1, %2;" : "=f"(fsum) : "f"(fa), "f"(fa));

        unsigned lane;
        asm volatile("mov.u32 %0, %%laneid;" : "=r"(lane));

        out[idx] = sum + static_cast<int>(lane);
        fout[idx] = fsum;
    }
}
