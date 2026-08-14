#include <stdexcept>
#include <string>
#include <vector>

#include "../CuAsm/CuInsParser.hpp"
#include "utils/TestUtilsCommon.hpp"

using CuAsm::CuInsParser;

/**
 * @brief Exercises CuAsm::CuInsParser against known sm_61 instructions. Previously this file just
 *        printed one parsed instruction with no pass/fail condition at all; a regression that
 *        silently mis-parsed the instruction would not have failed the build. Every expected
 *        value below was captured from a real run of this exact parser (not hand-derived from the
 *        parsing logic, which would risk re-deriving the same bug the test is meant to catch) and
 *        is now pinned down as a regression guard.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    // A 4-operand FFMA with a negative float immediate and .reuse modifiers on two operands.
    {
        CuInsParser cip("sm_61");
        auto [insKey, insVals, insModi] = cip.parse("FFMA R9, R3.reuse, -0.5, R2.reuse ;");
        t.check("FFMA key/operand-values/modifiers all match a known-correct real parse",
                insKey == "FFMA_R_R_FI_R" && insVals == std::vector<std::int64_t>{7, 9, 3, 258048, 2} &&
                    insModi == std::vector<std::string>{"0_FFMA", "2_reuse", "3_FINeg", "4_reuse"});
    }

    // A plain 4-register IADD3, unpredicated (predicate value 7 means "always true"/PT).
    {
        CuInsParser cip("sm_61");
        auto [insKey, insVals, insModi] = cip.parse("IADD3 R0, R1, R2, RZ ;");
        t.check("unpredicated IADD3 parses with predicate value 7 (PT) and RZ as operand 255",
                insKey == "IADD3_R_R_R_R" && insVals == std::vector<std::int64_t>{7, 0, 1, 2, 255} &&
                    insModi == std::vector<std::string>{"0_IADD3"});
    }

    // Predicate encoding: @P0 -> 0, @!P0 (negated) -> 0 + 8, both otherwise identical to unpredicated.
    {
        CuInsParser cip("sm_61");
        auto [key1, vals1, modi1] = cip.parse("@P0 IADD3 R0, R1, R2, RZ ;");
        auto [key2, vals2, modi2] = cip.parse("@!P0 IADD3 R0, R1, R2, RZ ;");
        t.check("@P0/@!P0 encode predicate value 0/8 (negated adds 8), same key/operands otherwise",
                key1 == "IADD3_R_R_R_R" && vals1[CuInsParser::PRED_VAL_IDX] == 0 && key2 == "IADD3_R_R_R_R" &&
                    vals2[CuInsParser::PRED_VAL_IDX] == 8);
    }

    // BRA's operand is a relative offset (target - next-instruction-address), not the raw target.
    {
        CuInsParser cip("sm_61");
        auto [insKey, insVals, insModi] = cip.parse("BRA 0x100 ;", /*addr=*/0);
        t.check("BRA's operand is target minus the next instruction's address (0x100 - 8 = 248 on sm_61)",
                insKey == "BRA_II" && insVals == std::vector<std::int64_t>{7, 248});
    }

    // Malformed input the parser can't recognize at all throws, rather than silently misparsing.
    {
        CuInsParser cip("sm_61");
        t.checkThrows<std::runtime_error>("parse() throws on an empty instruction string", [&] { (void)cip.parse(""); });
    }
    {
        CuInsParser cip("sm_61");
        t.checkThrows<std::runtime_error>("parse() throws on input that's only a semicolon", [&] { (void)cip.parse(";"); });
    }

    // stripModifier: separates an operand's dot-suffix modifier from its main text ...
    {
        CuInsParser cip("sm_61");
        const auto split = cip.stripModifier("R3.reuse");
        t.check("stripModifier splits R3.reuse into main text R3 and modifier 'reuse'",
                split.opMain == "R3" && split.opModi == std::vector<std::string>{"reuse"});
    }
    // ... and a leading '~' pre-modifier into the "cINV" (complement/invert) tag.
    {
        CuInsParser cip("sm_61");
        const auto split = cip.stripModifier("~R3");
        t.check("stripModifier translates a leading '~' into the cINV modifier tag",
                split.opMain == "R3" && split.opModi == std::vector<std::string>{"cINV"});
    }

    // dumpInfoAsDict() snapshots the most recent parse's internal state.
    {
        CuInsParser cip("sm_61");
        cip.parse("IADD3 R0, R1, R2, RZ ;", /*addr=*/0x40);
        const auto info = cip.dumpInfoAsDict();
        t.check("dumpInfoAsDict reflects the most recent parse's opcode, key, and address",
                info.insOp == "IADD3" && info.insKey == "IADD3_R_R_R_R" && info.insAddr == 0x40);
    }

    return t.finish("test_CuInsParser");
}
