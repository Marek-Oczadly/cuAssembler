// Single-kernel fixture for the CheckDisasm round-trip tests: exercises tensor core matrix
// multiply-accumulate via the WMMA API (fragment load, mma_sync, fragment store). Tensor cores
// require sm_70 (Volta) or newer, so this kernel is only compiled for those architectures; the
// corresponding sm_60/sm_61 tests skip themselves (see minArchSM in CheckDisasmCommon.hpp).
#include <mma.h>

using namespace nvcuda;

extern "C" __global__ void TensorCoreOps(const half* a, const half* b, const float* c, float* out)
{
    wmma::fragment<wmma::matrix_a, 16, 16, 16, half, wmma::row_major> fragA;
    wmma::fragment<wmma::matrix_b, 16, 16, 16, half, wmma::col_major> fragB;
    wmma::fragment<wmma::accumulator, 16, 16, 16, float> fragC;
    wmma::fragment<wmma::accumulator, 16, 16, 16, float> fragOut;

    wmma::load_matrix_sync(fragA, a, 16);
    wmma::load_matrix_sync(fragB, b, 16);
    wmma::load_matrix_sync(fragC, c, 16, wmma::mem_row_major);

    wmma::mma_sync(fragOut, fragA, fragB, fragC);

    wmma::store_matrix_sync(out, fragOut, 16, wmma::mem_row_major);
}
