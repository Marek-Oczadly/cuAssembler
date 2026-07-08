// Single-kernel fixture for the CheckDisasm round-trip tests: exercises control-flow
// instructions (branches, a bounded for-loop with an early break, and a while-loop).
extern "C" __global__ void ControlFlow(const int* in, int* out, int n, int limit)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n) {
        return;
    }

    int v = in[idx];
    int acc = 0;
    for (int i = 0; i < limit; ++i) {
        if (v % 2 == 0) {
            acc += i;
        } else {
            acc -= i;
        }

        v = v >> 1;
        if (v == 0) {
            break;
        }
    }

    while (acc > 100) {
        acc -= 100;
    }

    out[idx] = acc;
}
