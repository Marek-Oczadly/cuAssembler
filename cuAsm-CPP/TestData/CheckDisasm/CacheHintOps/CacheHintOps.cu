// Single-kernel fixture for the CheckDisasm round-trip tests: exercises explicit cache-hint
// load/store instructions (__ldg, __ldcs, __ldcg, __stcg, __stwt), not exercised by MemoryOps'
// plain unqualified global-memory access.
extern "C" __global__ void CacheHintOps(const float* in, float* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        float a = __ldg(&in[idx]);
        float b = __ldcs(&in[(idx + 1) % n]);
        float c = __ldcg(&in[(idx + 2) % n]);
        float sum = a + b + c;

        __stcg(&out[idx], sum);
        __stwt(&out[(idx + 1) % n], sum * 2.0f);
    }
}
