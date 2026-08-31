// Single-kernel fixture for the CheckDisasm round-trip tests: exercises calls to non-inlined
// __device__ functions (forced via __noinline__), including a function that itself calls
// another __device__ function, rather than everything being flattened into the kernel body.
__device__ __noinline__ int square(int x)
{
    return x * x;
}

__device__ __noinline__ int addOne(int x)
{
    return x + 1;
}

__device__ __noinline__ int combine(int a, int b)
{
    return square(a) + addOne(b);
}

extern "C" __global__ void DeviceFunctionCalls(const int* in, int* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        int v = in[idx];
        out[idx] = combine(v, v);
    }
}
