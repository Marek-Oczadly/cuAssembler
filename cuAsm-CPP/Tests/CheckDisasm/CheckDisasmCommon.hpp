#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "../../CuAsm/CubinFile.hpp"
#include "../../CuAsm/CuAsmParser.hpp"
#include "../../CuAsm/common.hpp"

namespace CuAsm::Test {

namespace fs = std::filesystem;

/**
 * @brief Reads an entire binary file into memory.
 * @param path Path of the file to read.
 * @return The file's raw byte contents.
 **/
inline std::string readBinaryFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("Failed to open file: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/**
 * @brief Compares two binary buffers byte-for-byte.
 * @param expected Reference buffer (the compiler's original cubin).
 * @param actual Buffer produced by reassembling the disassembly of expected.
 * @param diffDescription Set to a human-readable description of the first mismatch found,
 *        left unmodified if the buffers are identical.
 * @return true if expected and actual are byte-for-byte identical.
 **/
inline bool compareBinaries(const std::string& expected, const std::string& actual, std::string& diffDescription) {
    const std::size_t n = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (expected[i] != actual[i]) {
            const unsigned expByte = static_cast<unsigned char>(expected[i]);
            const unsigned actByte = static_cast<unsigned char>(actual[i]);
            std::ostringstream ss;
            ss << "binaries differ at byte offset 0x" << std::hex << i << ": expected 0x" << expByte
               << ", got 0x" << actByte << std::dec;
            diffDescription = ss.str();
            return false;
        }
    }

    if (expected.size() != actual.size()) {
        std::ostringstream ss;
        ss << "binaries differ in size: expected " << expected.size() << " bytes, got " << actual.size()
           << " bytes (first " << n << " bytes match)";
        diffDescription = ss.str();
        return false;
    }

    return true;
}

/**
 * @brief Removes a file if it exists, ignoring any error.
 * @param path Path of the file to remove.
 **/
inline void removeQuiet(const std::string& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

/**
 * @brief Runs the CheckDisasm round trip for one (kernel, architecture) pair: compiles the
 *        kernel's .cu fixture with nvcc, disassembles the resulting cubin with CubinFile,
 *        reassembles that disassembly with CuAsmParser, and checks that the reassembled cubin
 *        is byte-for-byte identical to nvcc's original. Any intermediate files left over from a
 *        previous run are cleared before this run starts; the files produced by this run are
 *        left in place afterward (pass or fail) for inspection, and get cleared on the next run.
 * @param kernelName Name of the kernel/subdirectory under TestData/CheckDisasm; also the base
 *        name of the .cu fixture and the entry-point symbol within it.
 * @param arch Target SM architecture, e.g. "sm_75".
 * @return true if the round trip reproduced a byte-identical cubin.
 **/
inline bool runCheckDisasm(const std::string& kernelName, const std::string& arch) {
    const std::string dir = std::string(CUASM_TESTDATA_DIR) + "/CheckDisasm/" + kernelName;
    const std::string cuFile = dir + "/" + kernelName + ".cu";
    const std::string origCubin = dir + "/" + kernelName + "." + arch + ".orig.cubin";
    const std::string cuasmFile = dir + "/" + kernelName + "." + arch + ".cuasm";
    const std::string newCubin = dir + "/" + kernelName + "." + arch + ".new.cubin";

    // Clear any leftover output from a previous run before starting a fresh one.
    removeQuiet(origCubin);
    removeQuiet(cuasmFile);
    removeQuiet(newCubin);

    bool passed = false;
    std::string failureReason;

    try {
        CuAsm::checkOutput({"nvcc", cuFile, "-arch=" + arch, "-cubin", "-o", origCubin}, true);

        CuAsm::CubinFile cf(origCubin);
        cf.saveAsCuAsm(cuasmFile);

        CuAsm::CuAsmParser cap;
        cap.parse(cuasmFile);
        cap.saveAsCubin(newCubin);

        const std::string expected = readBinaryFile(origCubin);
        const std::string actual = readBinaryFile(newCubin);

        if (compareBinaries(expected, actual, failureReason)) {
            passed = true;
            std::cout << "[PASS] CheckDisasm " << kernelName << " (" << arch
                      << "): reassembled cubin matches original (" << expected.size() << " bytes)\n";
        }
    } catch (const std::exception& e) {
        failureReason = e.what();
    }

    if (!passed) {
        std::cerr << "[FAIL] CheckDisasm " << kernelName << " (" << arch << "): " << failureReason << "\n";
    }

    return passed;
}

} // namespace CuAsm::Test
