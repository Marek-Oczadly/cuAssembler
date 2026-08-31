// Single-kernel fixture for the CheckDisasm round-trip tests: exercises a loop with internal
// conditionals via a naive (non-tiled) matrix multiplication.
extern "C" __global__ void MatrixMultiply(const float* A, const float* B, float* C, int m, int n, int k)
{
    int row = blockIdx.y * blockDim.y + threadIdx.y;
    int col = blockIdx.x * blockDim.x + threadIdx.x;

    if (row < m && col < n) {
        float acc = 0.0f;
        for (int i = 0; i < k; ++i) {
            float a = A[row * k + i];
            float b = B[i * n + col];
            if (a != 0.0f && b != 0.0f) {
                acc += a * b;
            } else if (a == 0.0f) {
                acc += 0.0f;
            } else {
                acc -= b;
            }
        }
        C[row * n + col] = acc;
    }
}
