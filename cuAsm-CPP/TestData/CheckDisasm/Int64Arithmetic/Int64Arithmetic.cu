// Single-kernel fixture for the CheckDisasm round-trip tests: exercises 64-bit integer
// arithmetic and shift instructions (wide add/multiply, 64-bit shifts/xor), distinct from
// IntegerArithmetic's 32-bit instruction encodings.
extern "C" __global__ void Int64Arithmetic(const long long* a, const long long* b, long long* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        long long x = a[idx];
        long long y = b[idx];

        long long sum = x + y;
        long long prod = x * y;
        long long shifted = (x << 3) ^ (y >> 2);

        out[idx] = sum + prod + shifted;
    }
}
