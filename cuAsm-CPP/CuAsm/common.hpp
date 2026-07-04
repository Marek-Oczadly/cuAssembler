#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
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

/**
 * @brief Pads a position up to the next multiple of an alignment, mirroring CuAsm.common.alignTo.
 * @param pos Current position.
 * @param align Alignment to pad to; 0 or 1 mean "no alignment" and return pos unchanged.
 * @return A pair of (aligned position, pad size added to reach it).
 **/
std::pair<std::uint64_t, std::uint64_t> alignTo(std::uint64_t pos, std::uint64_t align);

/**
 * @brief Formats a list of integers as a bracketed, comma-separated list of hex values,
 *        mirroring CuAsm.common.intList2Str.
 * @param vlist Values to format.
 * @param width If greater than zero, each hex value is zero-padded to this many characters
 *        (excluding the "0x" prefix), mirroring the python function's optional `l` parameter.
 * @return The formatted string, e.g. "[0x1, 0x2]".
 **/
std::string intList2Str(const std::vector<std::int64_t>& vlist, int width = 0);

/**
 * @brief Splits assembly text lines into sections according to ".section" directives,
 *        mirroring CuAsm.common.splitAsmSection.
 * @param lines Assembly text, split into lines.
 * @return A map from section name to a (start line, end line) pair. The header lines preceding
 *         the first section are stored under the key "$FileHeader".
 **/
std::map<std::string, std::pair<int, int>> splitAsmSection(const std::vector<std::string>& lines);

/**
 * @brief Converts a sequence of NUL-terminated byte strings into assembly ".byte" directives,
 *        mirroring CuAsm.common.stringBytes2Asm.
 * @param ss Byte buffer holding one or more NUL-terminated strings back to back.
 * @param label Name of this string list, used only for display in comments.
 * @param width Number of bytes to display per line.
 * @return The generated assembly text.
 **/
std::string stringBytes2Asm(const std::string& ss, const std::string& label = "", int width = 8);

/**
 * @brief Converts raw bytes into assembly ".byte" directives, mirroring CuAsm.common.bytes2Asm.
 * @param bs Byte buffer to convert.
 * @param width Max number of bytes to display per line.
 * @param addrOffset Address offset added to the displayed byte offset comment.
 * @param ident Indentation prepended to each line.
 * @return The generated assembly text.
 **/
std::string bytes2Asm(const std::string& bs, int width = 8, std::uint64_t addrOffset = 0, const std::string& ident = "    ");

/**
 * @brief Reads a binary file and writes its contents as assembly ".byte" directives, mirroring
 *        CuAsm.common.bytesdump.
 * @param inname Input binary file name.
 * @param outname Output assembly file name.
 **/
void bytesdump(const std::string& inname, const std::string& outname);

/**
 * @brief Strips cpp-style, c-style and bra-target comments from a line and collapses whitespace,
 *        mirroring CuAsm.common.stripComments.
 * @param s Line to strip.
 * @return The comment-free, whitespace-collapsed, trimmed line.
 **/
std::string stripComments(const std::string& s);

/**
 * @brief Writes a list to a stream in python repr-list style, mirroring CuAsm.common.reprList.
 *        Element type T must support operator<<(std::ostream&, const T&).
 * @param os Stream to write to.
 * @param list Values to write.
 **/
template <typename T>
void reprList(std::ostream& os, const std::vector<T>& list) {
    os << '[';
    for (std::size_t i = 0; i < list.size(); ++i) {
        os << list[i];
        if (i + 1 < list.size()) {
            os << ",\n";
        }
    }
    os << ']';
}

/**
 * @brief Writes a map to a stream in python repr-dict style, mirroring CuAsm.common.reprDict.
 *        Key/value types must support operator<<(std::ostream&, const T&).
 * @param os Stream to write to.
 * @param dict Map to write.
 **/
template <typename K, typename V>
void reprDict(std::ostream& os, const std::map<K, V>& dict) {
    os << '{';
    std::size_t i = 0;
    for (const auto& [k, v] : dict) {
        os << k << ':' << v;
        if (++i < dict.size()) {
            os << ",\n";
        }
    }
    os << '}';
}

/**
 * @brief Formats a matrix's entries as hex (for integral-valued entries) or decimal strings,
 *        right-aligned per column, mirroring CuAsm.common.reprHexMat. MatrixT must expose
 *        rows(), cols(), and operator()(int, int) returning an arithmetic value.
 * @param mat Matrix to format.
 * @return The formatted "Matrix([...])" string.
 **/
template <typename MatrixT>
std::string reprHexMat(const MatrixT& mat) {
    const int rows = static_cast<int>(mat.rows());
    const int cols = static_cast<int>(mat.cols());

    std::vector<std::vector<std::string>> smat(rows, std::vector<std::string>(cols));
    std::vector<std::size_t> colw(cols, 0);

    for (int j = 0; j < cols; ++j) {
        for (int i = 0; i < rows; ++i) {
            const auto val = mat(i, j);
            std::ostringstream cell;

            if constexpr (std::is_integral_v<std::decay_t<decltype(val)>>) {
                cell << "0x" << std::hex << val;
            } else if (std::floor(val) == val) {
                cell << "0x" << std::hex << static_cast<std::int64_t>(val);
            } else {
                cell << val;
            }

            smat[i][j] = cell.str();
            colw[j] = std::max(colw[j], smat[i][j].size());
        }
    }

    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            if (smat[i][j].size() < colw[j]) {
                smat[i][j] = std::string(colw[j] - smat[i][j].size(), ' ') + smat[i][j];
            }
        }
    }

    std::ostringstream sio;
    sio << "Matrix([\n";
    for (int i = 0; i < rows; ++i) {
        sio << '[';
        for (int j = 0; j < cols; ++j) {
            sio << smat[i][j];
            if (j + 1 < cols) {
                sio << ", ";
            }
        }
        sio << "],\n";
    }
    sio << "])";

    return sio.str();
}

} // namespace CuAsm
