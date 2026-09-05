#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <cuda.h>

#include "../../CuAsm/common.hpp"
#include "CuAsmTools/CorrectCC.hpp"

// Reports/correct-cc-insKey-drops-width-modifier.md, and the four scoreboard-correction bug
// reports before it: every one of those five bugs passed verify-cc's own self-check (internal
// consistency against this codebase's own hazard model) and was only ever caught because a
// *different* project (SASS-Shuffler) happened to launch the corrected cubin on real GPU hardware
// and diff its output against golden data. This suite closes that gap inside cuAssembler itself:
// compiles a real, scoreboard-slot-pressure-heavy kernel (TestData/CheckDisasm/TiledGemm -- the
// same tiled shared-memory GEMM SASS-Shuffler's example/gemm/ uses), round-trips it through
// correctCubinControlCodes() with NO reorder applied (mirroring exactly what
// SASS-Shuffler/RL/Reassemble.hpp does unconditionally to every kernel, including its unmutated
// baseline -- the scenario all five bugs were actually found in), and then actually launches both
// the original and corrected cubins on this machine's GPU via the CUDA Driver API, comparing their
// real output. A wrong scoreboard wait is invisible to any check confined to this codebase's own
// model of hardware behavior; it is not invisible to the hardware itself.
//
// Requires an actual CUDA-capable GPU on this machine (not just the CUDA Toolkit) -- the
// executable target itself is guarded by find_package(CUDAToolkit QUIET) at configure time
// (CMakeLists.txt), same as bin/cc-diff-run.cpp, so a machine with no Driver API library skips
// building this suite entirely rather than failing.

namespace CuAsm::Test {

namespace fs = std::filesystem;

/**
 * @brief Throws std::runtime_error with the CUDA driver's own error string if a driver call did
 *        not return CUDA_SUCCESS. Mirrors bin/cc-diff-run.cpp's own cuCheck() -- that file is a
 *        main()-only CLI tool, not a linkable library, so this small helper is duplicated here
 *        rather than shared.
 * @param result Return value of the driver call just made.
 * @param what Short label for the call, used in the thrown message.
 **/
inline void cuCheckLaunch(CUresult result, const std::string& what) {
    if (result == CUDA_SUCCESS) {
        return;
    }
    const char* errName = nullptr;
    const char* errStr = nullptr;
    cuGetErrorName(result, &errName);
    cuGetErrorString(result, &errStr);
    std::ostringstream oss;
    oss << what << " failed: " << (errName ? errName : "?") << " (" << (errStr ? errStr : "?") << ")";
    throw std::runtime_error(oss.str());
}

/// RAII CUDA Driver context, created once per test process and reused across every cubin this
/// suite loads/launches -- both the original and corrected cubin need to run against the same live
/// context/device for their outputs to be comparable under identical conditions.
class DriverContext {
public:
    DriverContext() {
        cuCheckLaunch(cuInit(0), "cuInit");
        CUdevice device;
        cuCheckLaunch(cuDeviceGet(&device, 0), "cuDeviceGet");
        cuCheckLaunch(cuCtxCreate(&m_ctx, 0, device), "cuCtxCreate");
    }
    ~DriverContext() { cuCtxDestroy(m_ctx); }
    DriverContext(const DriverContext&) = delete;
    DriverContext& operator=(const DriverContext&) = delete;

private:
    CUcontext m_ctx{};
};

/**
 * @brief Loads "gemm_tiled" from an in-memory cubin image and launches it once against the given
 *        A/B host matrices, returning the resulting C matrix actually computed by the GPU.
 * @param cubinBytes Raw cubin file bytes -- either the original nvcc/ptxas output or a
 *        correctCubinControlCodes()-patched copy of it. Both load identically via
 *        cuModuleLoadData; that's exactly the property this suite relies on to compare them.
 * @param A Row-major wA x wA host matrix (this suite only ever constructs square inputs).
 * @param B Row-major wA x wB host matrix.
 * @param wA A's width / C's row count, a multiple of GEMM_BLOCK_SIZE (16).
 * @param wB B's width / C's column count, also a multiple of 16.
 * @return Row-major wA x wB host matrix C = A * B, as actually computed on the GPU.
 * @throws std::runtime_error on any CUDA Driver API failure.
 **/
inline std::vector<float> launchGemmTiled(DriverContext&, const std::vector<char>& cubinBytes, const std::vector<float>& A,
                                           const std::vector<float>& B, int wA, int wB) {
    CUmodule module;
    cuCheckLaunch(cuModuleLoadData(&module, cubinBytes.data()), "cuModuleLoadData");
    CUfunction function;
    cuCheckLaunch(cuModuleGetFunction(&function, module, "gemm_tiled"), "cuModuleGetFunction(\"gemm_tiled\")");

    const std::size_t aBytes = A.size() * sizeof(float);
    const std::size_t bBytes = B.size() * sizeof(float);
    const std::size_t cBytes = static_cast<std::size_t>(wA) * static_cast<std::size_t>(wB) * sizeof(float);

    CUdeviceptr dA = 0, dB = 0, dC = 0;
    cuCheckLaunch(cuMemAlloc(&dA, aBytes), "cuMemAlloc(A)");
    cuCheckLaunch(cuMemAlloc(&dB, bBytes), "cuMemAlloc(B)");
    cuCheckLaunch(cuMemAlloc(&dC, cBytes), "cuMemAlloc(C)");
    cuCheckLaunch(cuMemcpyHtoD(dA, A.data(), aBytes), "cuMemcpyHtoD(A)");
    cuCheckLaunch(cuMemcpyHtoD(dB, B.data(), bBytes), "cuMemcpyHtoD(B)");
    cuCheckLaunch(cuMemsetD8(dC, 0, cBytes), "cuMemsetD8(C)");

    // Parameter order (C, A, B, wA, wB) matches TiledGemm.cu's gemm_tiled() signature exactly.
    void* params[] = {&dC, &dA, &dB, &wA, &wB};

    constexpr int c_BlockSize = 16;
    const unsigned int gridX = static_cast<unsigned int>(wB / c_BlockSize);
    const unsigned int gridY = static_cast<unsigned int>(wA / c_BlockSize);

    cuCheckLaunch(cuLaunchKernel(function, gridX, gridY, 1, c_BlockSize, c_BlockSize, 1, 0, nullptr, params, nullptr),
                  "cuLaunchKernel(\"gemm_tiled\")");
    cuCheckLaunch(cuCtxSynchronize(), "cuCtxSynchronize");

    std::vector<float> C(static_cast<std::size_t>(wA) * static_cast<std::size_t>(wB));
    cuCheckLaunch(cuMemcpyDtoH(C.data(), dC, cBytes), "cuMemcpyDtoH(C)");

    cuMemFree(dA);
    cuMemFree(dB);
    cuMemFree(dC);
    cuModuleUnload(module);
    return C;
}

/**
 * @brief Straightforward host-side reference GEMM, computed entirely independently of the GPU
 *        kernel -- used only as a sanity check that this suite's own fixture/harness computes what
 *        it thinks it does, separate from the actual regression check (GPU-original vs
 *        GPU-corrected output).
 **/
inline std::vector<float> referenceGemm(const std::vector<float>& A, const std::vector<float>& B, int wA, int wB) {
    std::vector<float> C(static_cast<std::size_t>(wA) * static_cast<std::size_t>(wB), 0.0f);
    for (int row = 0; row < wA; ++row) {
        for (int col = 0; col < wB; ++col) {
            float acc = 0.0f;
            for (int k = 0; k < wA; ++k) {
                acc += A[static_cast<std::size_t>(row) * wA + k] * B[static_cast<std::size_t>(k) * wB + col];
            }
            C[static_cast<std::size_t>(row) * wB + col] = acc;
        }
    }
    return C;
}

/// Result of compareMatrices(): whether every element of `actual` matched `expected` within
/// tolerance, plus enough detail to report a useful failure message.
struct CompareResult {
    bool ok = true;
    std::size_t mismatches = 0;
    float maxAbsDiff = 0.0f;
    std::size_t firstMismatchIndex = 0;
};

/**
 * @brief Element-wise comparison of two equal-length matrices with an `atol + rtol * |expected|`
 *        tolerance (mirroring CuAsmRL's own default float comparison shape).
 **/
inline CompareResult compareMatrices(const std::vector<float>& expected, const std::vector<float>& actual, float atol, float rtol) {
    CompareResult r;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const float diff = std::fabs(expected[i] - actual[i]);
        const float tol = atol + rtol * std::fabs(expected[i]);
        if (diff > tol) {
            if (r.mismatches == 0) {
                r.firstMismatchIndex = i;
            }
            ++r.mismatches;
            r.maxAbsDiff = std::max(r.maxAbsDiff, diff);
            r.ok = false;
        }
    }
    return r;
}

/**
 * @brief Runs the CheckControlCodesLaunch round trip for one (kernel, architecture) pair:
 *        compiles the fixture, corrects its control codes with NO reorder applied
 *        (correctCubinControlCodes() on the fixture's own untouched instruction order -- the same
 *        "unmutated baseline" round trip SASS-Shuffler/RL/Reassemble.hpp always performs), then
 *        actually launches the original and corrected cubins on real GPU hardware with identical,
 *        randomly-generated input matrices and compares their real output, plus an independent
 *        CPU-reference GEMM as a harness sanity check.
 * @param kernelName Name of the kernel/subdirectory under TestData/CheckDisasm (must define an
 *        `extern "C" __global__ void gemm_tiled(float* C, float* A, float* B, int wA, int wB)`).
 * @param arch Target SM architecture, e.g. "sm_75".
 * @param size Square matrix dimension to test with; must be a multiple of 16 (GEMM_BLOCK_SIZE).
 * @return true if correction succeeded, the harness's own CPU-reference sanity check passed, and
 *         the corrected cubin's real GPU output exactly matches the original cubin's real GPU
 *         output; false on any compile/correct/launch error, an unexpected Unrepairable result on
 *         the unmodified instruction order, a harness sanity-check failure, or an actual
 *         original-vs-corrected output mismatch (a real scoreboard-correction regression).
 **/
inline bool runCheckControlCodesLaunch(const std::string& kernelName, const std::string& arch, int size = 64) {
    bool passed = false;
    std::string failureReason;

    try {
        const std::string dir = std::string(CUASM_TESTDATA_DIR) + "/CheckDisasm/" + kernelName;
        const std::string cuFile = dir + "/" + kernelName + ".cu";
        const std::string origCubin = dir + "/" + kernelName + "." + arch + ".launch.cubin";
        const std::string correctedCubin = dir + "/" + kernelName + "." + arch + ".launch.corrected.cubin";

        std::error_code ec;
        fs::remove(origCubin, ec);
        fs::remove(correctedCubin, ec);

        CuAsm::checkOutput({"nvcc", cuFile, "-arch=" + arch, "-cubin", "-o", origCubin}, true);

        const CuAsm::Tools::CubinCorrectionReport report = CuAsm::Tools::correctCubinControlCodes(origCubin, correctedCubin);
        if (report.anyUnrepairable) {
            throw std::runtime_error(
                "correctCubinControlCodes() reported Unrepairable on the UNMODIFIED kernel (no reorder applied) -- "
                "ptxas's own control codes for this exact instruction order are already known-valid, so a "
                "from-scratch model that can't find any valid <=6-slot assignment for the same order is a bug in "
                "the correction model itself, not a real hardware limit (see "
                "Reports/correct-cc-shared-slot-reuse-not-modeled.md)");
        }
        if (!report.wrote) {
            throw std::runtime_error("correctCubinControlCodes() did not write a corrected cubin");
        }

        const auto readFile = [](const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                throw std::runtime_error("cannot open \"" + path + "\"");
            }
            return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        const std::vector<char> origBytes = readFile(origCubin);
        const std::vector<char> correctedBytes = readFile(correctedCubin);

        std::mt19937 rng(0xC0FFEEu);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        std::vector<float> A(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
        std::vector<float> B(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
        for (float& v : A) {
            v = dist(rng);
        }
        for (float& v : B) {
            v = dist(rng);
        }

        DriverContext ctx;
        const std::vector<float> cOrig = launchGemmTiled(ctx, origBytes, A, B, size, size);
        const std::vector<float> cCorrected = launchGemmTiled(ctx, correctedBytes, A, B, size, size);
        const std::vector<float> cReference = referenceGemm(A, B, size, size);

        // Sanity check: the harness/fixture itself must agree with an independent CPU reference --
        // if this fails, the bug is in this test, not in correct-cc.
        const CompareResult sanity = compareMatrices(cReference, cOrig, 1e-2f, 1e-3f);
        if (!sanity.ok) {
            throw std::runtime_error(
                "harness sanity check failed: the ORIGINAL (uncorrected) cubin's own GPU output already disagrees "
                "with an independent CPU-reference GEMM (" +
                std::to_string(sanity.mismatches) + " mismatch(es), max abs diff " + std::to_string(sanity.maxAbsDiff) +
                ") -- this is a fixture/harness bug, not a correct-cc regression");
        }

        // The actual regression check: correcting control codes must not change what the kernel
        // computes. Instruction order/arithmetic is untouched by control-code correction -- only
        // wait-barrier timing changes -- so the two outputs are expected to be exactly
        // bit-identical; the tiny tolerance below guards only against unrelated nondeterminism, not
        // because any real difference here would be acceptable.
        const CompareResult regression = compareMatrices(cOrig, cCorrected, 1e-6f, 0.0f);
        if (!regression.ok) {
            std::ostringstream oss;
            oss << "correctCubinControlCodes() changed the kernel's real GPU output on its UNMODIFIED instruction "
                << "order: " << regression.mismatches << "/" << cOrig.size() << " output element(s) differ (max abs "
                << "diff " << regression.maxAbsDiff << ", first mismatch at index " << regression.firstMismatchIndex
                << ", original=" << cOrig[regression.firstMismatchIndex]
                << " corrected=" << cCorrected[regression.firstMismatchIndex]
                << ") -- the corrected control codes are apparently self-consistent but wrong on real hardware, "
                   "exactly the class of bug documented in Reports/correct-cc-insKey-drops-width-modifier.md and its "
                   "predecessors";
            throw std::runtime_error(oss.str());
        }

        passed = true;
        std::cout << "[PASS] CheckControlCodesLaunch " << kernelName << " (" << arch << "): " << size << "x" << size
                   << " GEMM, original-vs-corrected-vs-CPU-reference all agree (" << report.kernels.size()
                   << " kernel(s) corrected)\n";
    } catch (const CuAsm::CalledProcessError& e) {
        failureReason = std::string(e.what()) + ": " + e.output();
    } catch (const std::exception& e) {
        failureReason = e.what();
    }

    if (!passed) {
        std::cerr << "[FAIL] CheckControlCodesLaunch " << kernelName << " (" << arch << "): " << failureReason << "\n";
    }

    return passed;
}

} // namespace CuAsm::Test
