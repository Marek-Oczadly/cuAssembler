#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "CuSMVersion.hpp"

namespace CuAsm {

/**
 * @brief Parses an instruction asm string into its key, operand values and modifiers, mirroring
 *        CuAsm.CuInsParser.CuInsParser. The result can then be assembled by CuInsAssembler.
 *
 * Since parsing is stateful (mirroring the python original's design, which keeps the last parse's
 * intermediate results around for inspection/debugging), a single CuInsParser should only be used
 * by one caller at a time; every parse() call resets all state before parsing.
 **/
class CuInsParser {
public:
    /// Index of the predicate value within m_InsVals, mirroring CuInsParser.PRED_VAL_IDX.
    static constexpr int PRED_VAL_IDX = 0;

    /// Index of the first real operand's value within m_InsVals, mirroring CuInsParser.OPERAND_VAL_IDX.
    static constexpr int OPERAND_VAL_IDX = 1;

    /**
     * @brief Constructs a parser bound to a particular arch.
     * @param arch Arch string (e.g. "sm_75"), valid as input to CuSMVersion.
     **/
    explicit CuInsParser(const std::string& arch = "sm_75");

    /**
     * @brief Resets all per-parse state, mirroring CuInsParser.reset. Called automatically at the
     *        start of parse().
     **/
    void reset();

    /**
     * @brief Parses an instruction asm string into its key, operand values, and modifiers.
     * @param s Instruction asm string to parse.
     * @param addr Address of the instruction, needed by branch-type instructions.
     * @param code Instruction code, recorded for later inspection via dumpInfo().
     * @return Tuple of (insKey, insVals, insModifiers).
     **/
    std::tuple<std::string, std::vector<std::int64_t>, std::vector<std::string>>
    parse(const std::string& s, std::uint64_t addr = 0, std::uint64_t code = 0);

    /**
     * @brief Prints the parser's internal state for the most recently parsed instruction to stdout.
     **/
    void dumpInfo() const;

    /// Plain (non-string-typed) snapshot of the parser's internal state, mirroring the dict
    /// returned by CuInsParser.dumpInfoAsDict.
    struct DumpInfoDict {
        std::string insString;
        std::string cTrString;
        std::uint64_t insAddr = 0;
        std::string insPredStr;
        std::int64_t insPredVal = 0;
        std::uint64_t insCode = 0;
        std::string insOp;
        std::string insOpFull;
        std::string insKey;
        std::vector<std::int64_t> insVals;
        std::vector<std::string> insTags;
        std::vector<std::string> insModi;
        std::vector<std::pair<std::string, std::int64_t>> rpList;
    };

    /**
     * @brief Snapshots the parser's internal state for the most recently parsed instruction,
     *        mirroring CuInsParser.dumpInfoAsDict.
     * @return The snapshot.
     **/
    DumpInfoDict dumpInfoAsDict() const;

    /// Result of stripModifier()/stripImmeModifier(): the operand text with modifiers stripped
    /// off, plus the list of modifier names found.
    struct ModifierSplit {
        std::string opMain;
        std::vector<std::string> opModi;
    };

    /**
     * @brief Splits a token into its pre-modifiers ("~-|!"), main text, and post-modifiers
     *        (".FTZ", ".X16", ...), mirroring CuInsParser.stripModifier.
     * @param s Token to split.
     * @return The main text and combined modifier list.
     **/
    ModifierSplit stripModifier(const std::string& s) const;

    /**
     * @brief Splits an immediate token into its value text and dot-separated modifiers, mirroring
     *        CuInsParser.stripImmeModifier.
     * @param s Immediate token to split.
     * @return The value text and modifier list.
     **/
    ModifierSplit stripImmeModifier(const std::string& s) const;

    /**
     * @brief Gets a process-wide shared parser for arch, constructing it on first use, mirroring
     *        CuInsParser.getStaticParser.
     * @param arch Arch string to get the shared parser for.
     * @return Reference to the shared parser instance for arch. Since parsers are stateful,
     *         callers sharing an instance must not interleave calls to parse().
     **/
    static CuInsParser& getStaticParser(const std::string& arch);

    CuSMVersion m_Arch;

    std::uint64_t m_InsAddr = 0;
    std::string m_InsString;
    std::string m_CTrString;
    std::uint64_t m_InsCode = 0;

    std::string m_InsKey;
    std::string m_InsOp;
    std::string m_InsOpFull;
    std::int64_t m_InsPredVal = 0;
    std::string m_InsPredStr;
    std::vector<std::string> m_InsModifier;
    std::vector<std::int64_t> m_InsVals;
    std::vector<std::string> m_InsTags;

    std::vector<std::pair<std::string, std::int64_t>> m_RPList;

private:
    /// Result of the various operand-shaped parse helpers (__parseOperand/__parseAddress/
    /// __parseConstMemory/__parseURConstMemory/__parseDescAddress), each returning
    /// (type, val, modi, tag) in the original.
    struct OperandParse {
        std::string type;
        std::vector<std::int64_t> val;
        std::vector<std::string> modi;
        std::vector<std::string> tag;
    };

    /// Result of __parseIndexedToken: (label, val, modi).
    struct IndexedToken {
        std::string label;
        std::vector<std::int64_t> val;
        std::vector<std::string> modi;
    };

    /// Result of __parseIntImme/__parseFloatImme: (val, modi).
    struct ImmeParse {
        std::vector<std::int64_t> val;
        std::vector<std::string> modi;
    };

    std::string constTr(const std::string& s) const;
    std::string preprocessOperands(const std::string& s) const;

    OperandParse parseOperand(const std::string& operand);
    IndexedToken parseIndexedToken(const std::string& s);
    std::int64_t parsePred(const std::string& s);
    ImmeParse parseFloatImme(const std::string& s) const;
    ImmeParse parseIntImme(const std::string& s) const;
    OperandParse parseConstMemory(const std::string& s);
    OperandParse parseURConstMemory(const std::string& s);
    OperandParse parseDescAddress(const std::string& s);
    std::string transScoreboardSet(const std::string& s) const;
    OperandParse parseAddress(const std::string& s);
    void specialTreatment();

    static std::map<int, CuInsParser> s_staticParsers;
};

} // namespace CuAsm
