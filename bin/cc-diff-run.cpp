// Generic CUDA Driver-API host launcher for control-code differential testing (Reports/tasks.md
// Phase 5's second, still-open checklist item: "compute-sanitizer --tool racecheck/synccheck
// differential testing ... as an outside-the-repo backstop for role/latency-table errors the
// static model can't self-detect"). See Reports/handoff-turing-gpu-session.md's "Item 2" section
// for the full design rationale.
//
// Nothing in this repo has ever actually launched a kernel on a GPU before this tool -- every
// existing cubin fixture (TestData/CuTest/cudatest.cu, TestData/CheckDisasm/*/*.cu) only ever gets
// compiled to a cubin and statically decoded/disassembled/reassembled, never run. This tool loads
// an arbitrary in-memory cubin via the CUDA Driver API (cuModuleLoadData), independent of whether
// it's nvcc's original output or correct-cc's patched bytes, and launches one kernel from it with a
// small CLI-specified set of scalar/vector/buffer arguments -- letting the *same* launcher run
// both the "before" (original ptxas-scheduled) and "after" (correct-cc-corrected) cubin uniformly,
// without needing to relink a runtime-API executable (see the handoff doc for why the runtime API's
// fatbinary-linking model rules that out).
//
// IMPORTANT SCOPE NOTE (confirmed with the user before building this, 2026-08-28): compute-
// sanitizer's --tool racecheck detects __shared__-memory hazards relative to explicit
// synchronization (bar.sync/__syncthreads()), and --tool synccheck detects illegal/divergent use
// of those same sync primitives -- both operate at the PTX/CUDA-memory-model level. SASS scoreboard
// control codes (the B/R/W wait-barrier bits this whole project's verify-cc/correct-cc compute) are
// a layer *below* that: pure hardware instruction-issue timing, invisible to PTX-level
// instrumentation. A wrong/missing scoreboard wait does not produce a race or an illegal sync call
// -- it produces silent data corruption (an instruction reads a register before the async op that
// fills it has actually completed; the read itself is perfectly "valid" from compute-sanitizer's
// point of view). So racecheck/synccheck runs against this tool are a legitimate but weak outside-
// the-repo smoke test (does the corrected cubin even load/launch/exit cleanly), NOT proof of
// scoreboard correctness. The --dump-arg mechanism below exists specifically to let a caller diff
// actual output-buffer bytes between an original-cubin run and a corrected-cubin run of the same
// kernel with identical inputs, which IS a check that could catch a real scoreboard-correction bug.
//
// Argument grammar (repeatable, in kernel-parameter order):
//     i32=<int>                          4-byte signed int parameter
//     u32=<uint>                         4-byte unsigned int parameter
//     f32=<float>                        4-byte float parameter
//     i32x4=<a>,<b>,<c>,<d>               16-byte int4 parameter
//     f32x4=<a>,<b>,<c>,<d>               16-byte float4 parameter
//     buf=<bytes>[,fill=<byte>]          Device buffer of <bytes> bytes, filled with the repeated
//                                         byte value <byte> (default 0) before launch; the kernel
//                                         parameter is the resulting device pointer.
//
// Example (TestData/CuTest/cudatest.cu's local_test, matching the handoff's recommended first
// kernel set):
//     cc-diff-run --cubin cudatest.7.sm_75.cubin --kernel local_test --grid 4,1,1 --block 64,1,1 \
//         --arg i32=2 --arg i32=5 --arg buf=1088,fill=0 --dump-arg 2:local_test.orig.bin

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <cuda.h>

namespace {

/**
 * @brief Throws std::runtime_error with the CUDA driver's own error string if a driver call did
 *        not return CUDA_SUCCESS.
 * @param result Return value of the driver call just made.
 * @param what Short label for the call, used in the thrown message.
 **/
void cuCheck(CUresult result, const std::string& what) {
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

/// One resolved kernel-parameter slot: the raw bytes cuLaunchKernel's kernelParams[i] must point
/// to, plus (for a "buf=" arg) the device pointer that owns the backing allocation, so it can be
/// freed and optionally read back after the launch.
struct ResolvedArg {
    std::vector<std::byte> hostBytes; ///< Raw parameter bytes cuLaunchKernel reads from.
    CUdeviceptr devicePtr = 0;        ///< Non-zero only for a "buf=" arg (the allocation itself).
    std::size_t bufBytes = 0;         ///< Non-zero only for a "buf=" arg (its byte size).
};

/** @brief Splits a comma-separated list of integers, e.g. "1,2,3,4" -> {1,2,3,4}. */
std::vector<long long> splitInts(const std::string& s) {
    std::vector<long long> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        out.push_back(std::stoll(tok));
    }
    return out;
}

/**
 * @brief Parses one "--arg" spec and resolves it to its raw kernel-parameter bytes, allocating and
 *        filling a device buffer for a "buf=" spec.
 * @param spec One argument spec, e.g. "i32=5" or "buf=1088,fill=0".
 * @return The resolved argument.
 * @throws std::runtime_error / std::invalid_argument on a malformed spec.
 **/
ResolvedArg resolveArg(const std::string& spec) {
    const std::size_t eq = spec.find('=');
    if (eq == std::string::npos) {
        throw std::runtime_error("malformed --arg \"" + spec + "\" (expected kind=value)");
    }
    const std::string kind = spec.substr(0, eq);
    const std::string rest = spec.substr(eq + 1);

    ResolvedArg arg;
    if (kind == "i32") {
        const std::int32_t v = static_cast<std::int32_t>(std::stoll(rest));
        arg.hostBytes.resize(sizeof(v));
        std::memcpy(arg.hostBytes.data(), &v, sizeof(v));
    } else if (kind == "u32") {
        const std::uint32_t v = static_cast<std::uint32_t>(std::stoull(rest));
        arg.hostBytes.resize(sizeof(v));
        std::memcpy(arg.hostBytes.data(), &v, sizeof(v));
    } else if (kind == "f32") {
        const float v = std::stof(rest);
        arg.hostBytes.resize(sizeof(v));
        std::memcpy(arg.hostBytes.data(), &v, sizeof(v));
    } else if (kind == "i32x4") {
        const std::vector<long long> vals = splitInts(rest);
        if (vals.size() != 4) {
            throw std::runtime_error("i32x4 needs exactly 4 comma-separated values: \"" + spec + "\"");
        }
        std::array<std::int32_t, 4> v{};
        for (std::size_t i = 0; i < 4; ++i) {
            v[i] = static_cast<std::int32_t>(vals[i]);
        }
        arg.hostBytes.resize(sizeof(v));
        std::memcpy(arg.hostBytes.data(), v.data(), sizeof(v));
    } else if (kind == "f32x4") {
        const std::vector<long long> vals = splitInts(rest); // integral literals are fine for our fixtures
        if (vals.size() != 4) {
            throw std::runtime_error("f32x4 needs exactly 4 comma-separated values: \"" + spec + "\"");
        }
        std::array<float, 4> v{};
        for (std::size_t i = 0; i < 4; ++i) {
            v[i] = static_cast<float>(vals[i]);
        }
        arg.hostBytes.resize(sizeof(v));
        std::memcpy(arg.hostBytes.data(), v.data(), sizeof(v));
    } else if (kind == "buf") {
        std::size_t bytes = 0;
        int fill = 0;
        std::stringstream ss(rest);
        std::string tok;
        bool first = true;
        while (std::getline(ss, tok, ',')) {
            if (first) {
                bytes = static_cast<std::size_t>(std::stoull(tok));
                first = false;
            } else if (tok.rfind("fill=", 0) == 0) {
                fill = std::stoi(tok.substr(5));
            }
        }
        if (bytes == 0) {
            throw std::runtime_error("buf= needs a nonzero byte size: \"" + spec + "\"");
        }
        cuCheck(cuMemAlloc(&arg.devicePtr, bytes), "cuMemAlloc(" + spec + ")");
        cuCheck(cuMemsetD8(arg.devicePtr, static_cast<unsigned char>(fill), bytes), "cuMemsetD8(" + spec + ")");
        arg.bufBytes = bytes;
        arg.hostBytes.resize(sizeof(CUdeviceptr));
        std::memcpy(arg.hostBytes.data(), &arg.devicePtr, sizeof(CUdeviceptr));
    } else {
        throw std::runtime_error("unknown --arg kind \"" + kind + "\" in \"" + spec + "\"");
    }
    return arg;
}

/** @brief Parses "<index>:<path>" for --dump-arg. */
std::pair<std::size_t, std::string> parseDumpSpec(const std::string& spec) {
    const std::size_t colon = spec.find(':');
    if (colon == std::string::npos) {
        throw std::runtime_error("malformed --dump-arg \"" + spec + "\" (expected index:path)");
    }
    return {static_cast<std::size_t>(std::stoull(spec.substr(0, colon))), spec.substr(colon + 1)};
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"Load an arbitrary cubin via the CUDA Driver API and launch one kernel from it, "
                  "for control-code correction differential testing (Reports/tasks.md Phase 5)."};

    std::string cubinPath;
    std::string kernelName;
    std::string gridSpec = "1,1,1";
    std::string blockSpec = "1,1,1";
    unsigned int sharedMemBytes = 0;
    std::vector<std::string> argSpecs;
    std::vector<std::string> dumpSpecs;

    app.add_option("--cubin", cubinPath, "Path to the cubin to load")->required();
    app.add_option("--kernel", kernelName, "Kernel (__global__ function) name to launch")->required();
    app.add_option("--grid", gridSpec, "Grid dimensions as x,y,z")->default_val("1,1,1");
    app.add_option("--block", blockSpec, "Block dimensions as x,y,z")->default_val("1,1,1");
    app.add_option("--shmem", sharedMemBytes, "Dynamic shared memory bytes")->default_val(0);
    app.add_option("--arg", argSpecs, "One kernel parameter, in order (see grammar in file header)");
    app.add_option("--dump-arg", dumpSpecs,
                    "index:path -- after the launch, copy a \"buf=\" arg's device buffer back to "
                    "host and write its raw bytes to path");

    CLI11_PARSE(app, argc, argv);

    try {
        std::ifstream in(cubinPath, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot open cubin \"" + cubinPath + "\"");
        }
        const std::vector<char> cubinBytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        const std::vector<long long> grid = splitInts(gridSpec);
        const std::vector<long long> block = splitInts(blockSpec);
        if (grid.size() != 3 || block.size() != 3) {
            throw std::runtime_error("--grid/--block need exactly 3 comma-separated values");
        }

        cuCheck(cuInit(0), "cuInit");
        CUdevice device;
        cuCheck(cuDeviceGet(&device, 0), "cuDeviceGet");
        CUcontext ctx;
        cuCheck(cuCtxCreate(&ctx, 0, device), "cuCtxCreate");

        CUmodule module;
        cuCheck(cuModuleLoadData(&module, cubinBytes.data()), "cuModuleLoadData(\"" + cubinPath + "\")");
        CUfunction function;
        cuCheck(cuModuleGetFunction(&function, module, kernelName.c_str()), "cuModuleGetFunction(\"" + kernelName + "\")");

        std::vector<ResolvedArg> args;
        args.reserve(argSpecs.size());
        for (const std::string& spec : argSpecs) {
            args.push_back(resolveArg(spec));
        }
        std::vector<void*> kernelParams;
        kernelParams.reserve(args.size());
        for (ResolvedArg& a : args) {
            kernelParams.push_back(a.hostBytes.data());
        }

        cuCheck(cuLaunchKernel(function, static_cast<unsigned int>(grid[0]), static_cast<unsigned int>(grid[1]),
                                static_cast<unsigned int>(grid[2]), static_cast<unsigned int>(block[0]),
                                static_cast<unsigned int>(block[1]), static_cast<unsigned int>(block[2]), sharedMemBytes,
                                nullptr, kernelParams.data(), nullptr),
                "cuLaunchKernel(\"" + kernelName + "\")");
        cuCheck(cuCtxSynchronize(), "cuCtxSynchronize");

        for (const std::string& dumpSpec : dumpSpecs) {
            const auto [index, path] = parseDumpSpec(dumpSpec);
            if (index >= args.size() || args[index].bufBytes == 0) {
                throw std::runtime_error("--dump-arg index " + std::to_string(index) + " is not a \"buf=\" argument");
            }
            std::vector<std::byte> hostOut(args[index].bufBytes);
            cuCheck(cuMemcpyDtoH(hostOut.data(), args[index].devicePtr, args[index].bufBytes), "cuMemcpyDtoH(--dump-arg)");
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                throw std::runtime_error("cannot open dump output \"" + path + "\"");
            }
            out.write(reinterpret_cast<const char*>(hostOut.data()), static_cast<std::streamsize>(hostOut.size()));
        }

        for (ResolvedArg& a : args) {
            if (a.devicePtr != 0) {
                cuMemFree(a.devicePtr);
            }
        }
        cuModuleUnload(module);
        cuCtxDestroy(ctx);

        std::cout << "[OK] launched \"" << kernelName << "\" from \"" << cubinPath << "\" (grid " << gridSpec << ", block "
                   << blockSpec << ")\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
