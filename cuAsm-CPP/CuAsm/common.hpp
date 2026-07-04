#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace CuAsm {

/**
 * @brief Error thrown by checkOutput() when the launched process exits with a non-zero status,
 *        mirroring python's subprocess.CalledProcessError.
 **/
class CalledProcessError : public std::runtime_error {
public:
    /**
     * @brief Constructs the error with the process's exit code and captured output.
     * @param returnCode Exit code returned by the failed process.
     * @param output Captured stdout/stderr of the failed process.
     **/
    CalledProcessError(int returnCode, std::string output);

    /**
     * @brief Exit code returned by the failed process.
     * @return The process's exit code.
     **/
    int returnCode() const noexcept;

    /**
     * @brief Captured stdout/stderr of the failed process.
     * @return The process's captured output.
     **/
    const std::string& output() const noexcept;

private:
    int m_returnCode;
    std::string m_output;
};

/**
 * @brief Runs a command and captures its combined stdout/stderr, mirroring python's
 *        subprocess.check_output().
 * @param args Command and arguments, with args[0] as the executable name.
 * @return The captured output of the process.
 **/
std::string checkOutput(const std::vector<std::string>& args);

/**
 * @brief Gets a temporary filename in the system temp dir, mirroring CuAsm.common.getTempFileName.
 * @param name If non-empty, used verbatim as the temp file's base name instead of a random one.
 * @param prefix Prefix prepended to the base name.
 * @param suffix Suffix (extension) appended to the base name.
 * @return The generated temporary file path.
 **/
std::string getTempFileName(const std::string& name = "", const std::string& prefix = "cuasm", const std::string& suffix = "");

/**
 * @brief Formats an integer as a space-grouped binary string, mirroring CuAsm.common.binstr.
 * @param v Value to format.
 * @param bitlen Total number of bits to print, zero-padded on the left.
 * @param width Number of bits per group.
 * @param sp Separator inserted between groups.
 * @return The formatted binary string.
 **/
std::string binstr(std::uint64_t v, int bitlen = 128, int width = 4, const std::string& sp = " ");

/**
 * @brief Formats an integer as a space-grouped hex string, mirroring CuAsm.common.hexstr.
 * @param v Value to format.
 * @param bitlen Total number of bits to print (bitlen/4 hex digits), zero-padded on the left.
 * @param width Number of hex digits per group.
 * @param sp Separator inserted between groups.
 * @return The formatted hex string.
 **/
std::string hexstr(std::uint64_t v, int bitlen = 128, int width = 4, const std::string& sp = " ");

} // namespace CuAsm
