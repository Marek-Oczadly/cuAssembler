// Single-kernel fixture for the CheckDisasm round-trip tests: exercises integer arithmetic and
// bitwise instructions (add/sub/mul/div/mod, shifts, bitwise and/or/xor).
extern "C" __global__ void IntegerArithmetic(const int* a, const int* b, int* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        int x = a[idx];
        int y = b[idx];

        int sum = x + y;
        int diff = x - y;
        int prod = x * y;
        int quot = (y != 0) ? x / y : 0;
        int rem = (y != 0) ? x % y : 0;
        int bits = ((x << 2) ^ (y >> 1)) | (x & y);

        out[idx] = sum + diff + prod + quot + rem + bits;
    }
}
