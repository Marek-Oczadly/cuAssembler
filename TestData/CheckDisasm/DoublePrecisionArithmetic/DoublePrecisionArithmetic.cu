// Single-kernel fixture for the CheckDisasm round-trip tests: exercises double-precision
// floating-point instructions (fused multiply-add, division, square root, absolute value),
// distinct from FloatingPointArithmetic's single-precision instruction encodings.
extern "C" __global__ void DoublePrecisionArithmetic(const double* a, const double* b, double* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        double x = a[idx];
        double y = b[idx];

        double s = fma(x, y, x);
        double d = x / (y + 1.0);
        double r = sqrt(fabs(x));

        out[idx] = s + d + r;
    }
}
