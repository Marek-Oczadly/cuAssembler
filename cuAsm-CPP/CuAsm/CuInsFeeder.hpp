#pragma once

#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <string>

namespace CuAsm {

/**
 * @brief One (addr, code, asm, ctrl) record yielded while iterating a CuInsFeeder.
 **/
struct CuInsRecord {
    std::uint64_t addr;
    std::uint64_t code;
    std::string asmText;
    std::uint32_t ctrl;
};

class CuInsFeeder {
public:
    /**
     * @brief Constructs an instruction feeder reading from a named sass file, yielding
     *        (addr, code, asm, ctrl) tuples as it is iterated.
     * @param fileName Path of the sass/dumped-sass file to read.
     * @param archFilter Optional arch (valid CuSMVersion input) to restrict feeding to; empty means all arches.
     * @param insFilter Optional regex pattern or filter string used to select particular instructions; empty means no filtering.
     **/
    explicit CuInsFeeder(const std::string& fileName, const std::string& archFilter = "", const std::string& insFilter = "");

    /**
     * @brief Constructs an instruction feeder reading from an already-open stream, yielding
     *        (addr, code, asm, ctrl) tuples as it is iterated.
     * @param stream Input stream containing sass/dumped-sass text.
     * @param archFilter Optional arch (valid CuSMVersion input) to restrict feeding to; empty means all arches.
     * @param insFilter Optional regex pattern or filter string used to select particular instructions; empty means no filtering.
     **/
    explicit CuInsFeeder(std::istream& stream, const std::string& archFilter = "", const std::string& insFilter = "");

    /**
     * @brief Translates the fed sass into sass annotated with scoreboard control codes, writing
     *        the result to a named file.
     * @param outFileName Path of the output file to write.
     * @param codeOnlyLineMode "keep" to retain code-only lines (e.g. SM5x/6x control code lines) unchanged,
     *        "none" (default) to strip them for a more compact output.
     **/
    void trans(const std::string& outFileName, const std::string& codeOnlyLineMode = "none");

    /**
     * @brief Translates the fed sass into sass annotated with scoreboard control codes, writing
     *        the result to an already-open output stream.
     * @param outStream Output stream to write the translated sass to.
     * @param codeOnlyLineMode "keep" to retain code-only lines (e.g. SM5x/6x control code lines) unchanged,
     *        "none" (default) to strip them for a more compact output.
     **/
    void trans(std::ostream& outStream, const std::string& codeOnlyLineMode = "none");

    /**
     * @brief Rewinds the underlying stream back to the start, resetting the feeder's line counter
     *        so it can be iterated again from the beginning.
     **/
    void restart();

    /**
     * @brief Retrieves the next (addr, code, asm, ctrl) instruction record, honoring the arch and
     *        instruction filters, mirroring the original's __iter__ generator.
     * @return The next record, or std::nullopt once the feeder is exhausted.
     **/
    std::optional<CuInsRecord> next();

    // Name of the function/kernel currently being fed, updated as "Function :" / ".section .text." lines are read.
    std::string CurrFuncName;

    // Arch of the section currently being fed, updated as ".headerflags" lines are read.
    std::string CurrArch;
};

} // namespace CuAsm
