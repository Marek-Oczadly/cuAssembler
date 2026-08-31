// Single-kernel fixture for the CheckDisasm round-trip tests: exercises bindless texture-object
// memory access (tex1Dfetch), a memory-instruction family (TLD/TEX) not covered by MemoryOps'
// plain global/shared loads and stores.
extern "C" __global__ void TextureOps(cudaTextureObject_t tex, float* out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        out[idx] = tex1Dfetch<float>(tex, idx);
    }
}
