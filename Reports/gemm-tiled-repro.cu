// A tiled, shared-memory GEMM kernel -- example/README.md's non-automated "does the RL agent
// actually help on something more realistic than TestData/vecadd" case (loops, __syncthreads(),
// shared-memory LDS/STS traffic, none of which TestData/vecadd/vecadd.cu exercises at all).
//
// Adapted from NVIDIA's cuda-samples "matrixMul" example:
//   https://github.com/NVIDIA/cuda-samples/blob/master/cpp/0_Introduction/matrixMul/matrixMul.cu
// (BSD-3-Clause; full original license reproduced below, as its terms require). Adaptation for
// SASS-Shuffler, beyond deleting the unused host-side benchmark/verification driver code (this
// project's own reassemble/launch/benchmark/verify pipeline replaces all of that):
//   - The original is a function template parameterized on BLOCK_SIZE, instantiated at its two call
//     sites for either 16 or 32. Here it's instantiated once, concretely, at BLOCK_SIZE=16 (see
//     GEMM_BLOCK_SIZE below) -- SASS-Shuffler.exe's own kernel lookup (KernelParams.hpp) needs one
//     concrete, non-template kernel, not something resolved via host-side template instantiation.
//   - Wrapped in `extern "C"` so the compiled symbol is a plain, unmangled name ("gemm_tiled") --
//     matching TestData/vecadd/vecadd.cu's own `extern "C"` kernel, and required by
//     KernelParams.hpp's cuobjdump-based ".nv.info.<name>" section lookup.
// The tiling algorithm itself (shared-memory staging of A/B sub-tiles, the k-reduction loop, the two
// __syncthreads() barriers) is unmodified.
//
// ===== Original license (BSD-3-Clause) =====
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// ============================================================================

#define GEMM_BLOCK_SIZE 16

// C = A * B. A is (M x wA), B is (wA x wB), C is (M x wB) -- all row-major, flattened. M is implicit
// in the launch's grid.y (see run_example.sh's --grid/--block and generate_fixtures.cpp's shapes).
// Parameter order (C, A, B, wA, wB) matches the upstream kernel's own calling convention exactly.
extern "C" __global__ void gemm_tiled(float *C, float *A, float *B, int wA, int wB)
{
    int bx = blockIdx.x;
    int by = blockIdx.y;
    int tx = threadIdx.x;
    int ty = threadIdx.y;

    int aBegin = wA * GEMM_BLOCK_SIZE * by;
    int aEnd   = aBegin + wA - 1;
    int aStep  = GEMM_BLOCK_SIZE;

    int bBegin = GEMM_BLOCK_SIZE * bx;
    int bStep  = GEMM_BLOCK_SIZE * wB;

    float Csub = 0;

    for (int a = aBegin, b = bBegin; a <= aEnd; a += aStep, b += bStep) {
        __shared__ float As[GEMM_BLOCK_SIZE][GEMM_BLOCK_SIZE];
        __shared__ float Bs[GEMM_BLOCK_SIZE][GEMM_BLOCK_SIZE];

        As[ty][tx] = A[a + wA * ty + tx];
        Bs[ty][tx] = B[b + wB * ty + tx];

        __syncthreads();

#pragma unroll
        for (int k = 0; k < GEMM_BLOCK_SIZE; ++k) {
            Csub += As[ty][k] * Bs[k][tx];
        }

        __syncthreads();
    }

    int c               = wB * GEMM_BLOCK_SIZE * by + GEMM_BLOCK_SIZE * bx;
    C[c + wB * ty + tx] = Csub;
}
