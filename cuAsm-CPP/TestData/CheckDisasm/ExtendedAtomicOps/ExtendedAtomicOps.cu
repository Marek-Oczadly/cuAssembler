// Single-kernel fixture for the CheckDisasm round-trip tests: exercises atomic instructions not
// covered by AtomicOps (atomicExch, atomicAnd, atomicOr, atomicXor, atomicInc, atomicDec, and
// double-precision atomicAdd).
extern "C" __global__ void ExtendedAtomicOps(int* flags, unsigned int* bits, double* sums, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        atomicExch(&flags[idx % 32], idx);
        atomicAnd(&bits[idx % 32], 0xFF00FF00u);
        atomicOr(&bits[(idx + 1) % 32], 0x000000FFu);
        atomicXor(&bits[(idx + 2) % 32], 0xFFFFFFFFu);
        atomicInc(reinterpret_cast<unsigned int*>(&flags[(idx + 3) % 32]), 1000u);
        atomicDec(reinterpret_cast<unsigned int*>(&flags[(idx + 4) % 32]), 1000u);
        atomicAdd(&sums[idx % 32], 1.0);
    }
}
