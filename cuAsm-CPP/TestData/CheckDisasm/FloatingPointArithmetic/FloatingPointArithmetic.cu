// Single-kernel fixture for the CheckDisasm round-trip tests: exercises floating-point
// instructions (fused multiply-add, division, square root, reciprocal square root).
extern "C" __global__ void FloatingPointArithmetic(const float* a, const float* b, float* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        float x = a[idx];
        float y = b[idx];

        float s = fmaf(x, y, x);
        float d = x / (y + 1.0f);
        float r = sqrtf(fabsf(x)) + rsqrtf(fabsf(y) + 1.0f);

        out[idx] = s + d + r;
    }
}
