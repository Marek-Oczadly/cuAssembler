#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "CuSMVersion.hpp"
#include "utils/BigNum.hpp"

namespace CuAsm {

/**
 * @brief One (addr, code, asm, ctrl) record yielded while iterating a CuInsFeeder.
 **/
struct CuInsRecord {
    std::uint64_t addr;
    BigInt code;
    std::string asmText;
    std::uint32_t ctrl;
};

/**
 * @brief Parses a dumped sass file (as produced by `cuobjdump -sass`) into a stream of
 *        (addr, code, asm, ctrl) instruction records, mirroring CuAsm.CuInsFeeder.CuInsFeeder. Sass
 *        text is scanned line-by-line through a small per-arch-family state machine (mirroring the
 *        original's StateTransferMatrix-driven design, reimplemented here as plain switch-based
 *        transitions - idiomatic C++ has no easy equivalent of python's dynamically-typed
 *        {state: {op: (nextState, callback)}} tables) that groups raw instruction/code lines back
 *        into full instruction records, splitting off scoreboard control codes per arch.
 **/
class CuInsFeeder {
public:
    /**
     * @brief Constructs an instruction feeder reading from a named sass file, yielding
     *        (addr, code, asm, ctrl) tuples as it is iterated.
     * @param fileName Path of the sass/dumped-sass file to read.
     * @param archFilter Optional arch (valid CuSMVersion input) to restrict feeding to; empty means all arches.
     * @param insFilter Optional regex pattern used to select particular instructions; empty means no filtering.
     **/
    explicit CuInsFeeder(const std::string& fileName, const std::string& archFilter = "", const std::string& insFilter = "");

    /**
     * @brief Constructs an instruction feeder reading from an already-open stream, yielding
     *        (addr, code, asm, ctrl) tuples as it is iterated.
     * @param stream Input stream containing sass/dumped-sass text.
     * @param archFilter Optional arch (valid CuSMVersion input) to restrict feeding to; empty means all arches.
     * @param insFilter Optional regex pattern used to select particular instructions; empty means no filtering.
     **/
    explicit CuInsFeeder(std::istream& stream, const std::string& archFilter = "", const std::string& insFilter = "");

    /**
     * @brief Translates the fed sass into sass annotated with scoreboard control codes, writing
     *        the result to a named file. NOTE: unlike next(), this ignores both the arch and
     *        instruction filters and always restarts from the beginning of the stream, mirroring
     *        CuInsFeeder.trans.
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
     * @brief Dumps the first kernel (delimited by "Function :" lines) whose name matches
     *        funcFilter, or that contains an instruction matching insFilter, to a file, mirroring
     *        CuInsFeeder.extract. Does not restart the stream first; continues from wherever the
     *        feeder currently is.
     * @param outFileName Path of the file to write the matched kernel's sass to.
     * @param funcFilter Optional regex pattern for the kernel/function name; std::nullopt disables
     *        matching by name (mirroring func_filter=None). An explicit empty pattern matches every
     *        name, so the very first kernel encountered gets dumped.
     * @param insFilter Optional regex pattern for an instruction/code line; std::nullopt or empty
     *        means no instruction-based matching.
     **/
    void extract(const std::string& outFileName, std::optional<std::string> funcFilter = std::nullopt,
                 std::optional<std::string> insFilter = std::nullopt);

    /**
     * @brief Rewinds the underlying stream back to the start, resetting the feeder's line counter
     *        so it can be iterated again from the beginning, mirroring CuInsFeeder.restart. Unlike
     *        the python original (whose equivalent guard is unconditionally true due to a bug -
     *        `self.__mFStream.seekable` rather than `.seekable()`), this actually throws if the
     *        underlying stream turns out not to be seekable.
     **/
    void restart();

    /**
     * @brief Retrieves the next (addr, code, asm, ctrl) instruction record, honoring the arch and
     *        instruction filters, mirroring the original's __iter__ generator. Like __iter__
     *        calling restart() at the start of every fresh `for rec in feeder` loop, this
     *        auto-restarts the feeder when called for the first time or right after a previous
     *        iteration ran to exhaustion, so the feeder can simply be reused for another pass.
     * @return The next record, or std::nullopt once the feeder is exhausted.
     **/
    std::optional<CuInsRecord> next();

    /**
     * @brief Closes the underlying stream if this feeder owns it (i.e. was constructed from a
     *        file name), mirroring CuInsFeeder.close. A no-op returning false for feeders
     *        constructed from an externally-owned stream, or if already closed.
     * @return True if a stream was actually closed.
     **/
    bool close();

    /**
     * @brief Reports the underlying stream's read position, mirroring CuInsFeeder.tell.
     * @return The current stream position.
     **/
    std::streampos tell() const;

    /**
     * @brief Reports the current 1-based line number, mirroring CuInsFeeder.tellLine.
     * @return The number of lines read so far.
     **/
    std::uint64_t tellLine() const;

    /// Name of the function/kernel currently being fed, updated as "Function :" / ".section .text." lines are read.
    std::string CurrFuncName;

    /// Arch of the section currently being fed, updated as ".headerflags" lines are read.
    std::string CurrArch;

private:
    /// The six kinds of lines found in a dumped sass file, mirroring SassLineType.
    enum class LineType { InsCode, InsOnly, CodeOnly, FuncName, SectionName, HeaderFlag, Others };

    /// States of the per-line-group parser, mirroring the (non-dead) values of ParserState.
    enum class ParserState { Ready, WaitForCode3, WaitForCode2, WaitForCode1, WaitForIns3, WaitForIns2, WaitForIns1 };

    /// Which per-arch state machine/code-splitting rules are active, mirroring how __switchArch
    /// swaps out __CurrTM/__SplitCodeList. "Default" is active before the first ".headerflags" line.
    enum class ArchFamily { Default, Sm3x, Sm5x6x, Sm7x8x };

    /// A single classified+parsed sass line, mirroring the (linetype, line, res) returned by
    /// nextParseLine plus the args SassLineType.getCallbackArgs would extract from it.
    struct ParsedLine {
        LineType type = LineType::Others;
        std::string text;
        std::uint64_t addr = 0;
        std::string asmText;
        BigInt code;
        std::string func;
        std::string sec;
        std::string archDigits;
    };

    void initFilters(const std::string& archFilter, const std::string& insFilter);
    static std::function<bool(const std::string&)> makeFilterFun(const std::string& pattern);

    std::optional<ParsedLine> nextParseLine();
    static ParsedLine matchLineType(const std::string& line);

    ParserState feedLineOp(const ParsedLine& pl);
    ParserState feedDefault(const ParsedLine& pl);
    ParserState feedSm3x(const ParsedLine& pl);
    ParserState feedSm5x6x(const ParsedLine& pl);
    ParserState feedSm7x8x(const ParsedLine& pl);
    [[noreturn]] void throwInvalidOp(LineType op) const;

    void setFuncName(const std::string& func);
    void setSectionName(const std::string& sec);
    void switchArch(const std::string& archDigits);
    void emitMessage(const std::string& msg) const;

    void pushAddr(std::uint64_t addr);
    void pushAsm(const std::string& asmText);
    void pushCode(const BigInt& code);
    void pushInsCode(std::uint64_t addr, const std::string& asmText, const BigInt& code);
    void pushInsOnly(std::uint64_t addr, const std::string& asmText);

    std::vector<CuInsRecord> iterPopIns();
    bool filterIns(const std::string& asmText) const;

    std::string formatCtrlCodeString(std::uint32_t ccode, bool phantomMode = false) const;

    std::string m_FileName;
    std::ifstream m_OwnedFile;
    std::istream* m_Stream = nullptr;

    std::function<bool(const std::string&)> m_InsFilterFun;
    std::optional<CuSMVersion> m_ArchFilter;

    std::uint64_t m_LineNo = 0;

    std::optional<CuSMVersion> m_CurrArchVersion;
    ArchFamily m_ArchFamily = ArchFamily::Default;
    ParserState m_PState = ParserState::Ready;

    std::vector<std::uint64_t> m_AddrList;
    std::vector<std::string> m_AsmList;
    std::vector<BigInt> m_CodeList;

    // next()-resumable iteration state (mirrors the local `doFeed` variable + partially-consumed
    // yields of the original's __iter__ generator).
    bool m_DoFeed = false;
    std::vector<CuInsRecord> m_PendingRecords;
    std::size_t m_PendingIdx = 0;

    // Whether a next()-driven iteration is currently in progress. Mirrors __iter__ calling
    // restart() at the start of every fresh `for rec in feeder` loop: next() auto-restarts
    // whenever it's called while this is false (construction, or after a prior iteration ran
    // to exhaustion), so reusing the feeder doesn't silently yield nothing.
    bool m_IterationStarted = false;
};

} // namespace CuAsm
