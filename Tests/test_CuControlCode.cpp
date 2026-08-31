#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>

#include "../CuAsm/CuControlCode.hpp"
#include "utils/TestUtilsCommon.hpp"

using CuAsm::CuControlCode;

/**
 * @brief Exercises CuAsm::CuControlCode, the bit-packed stall/yield/read-barrier/write-barrier/
 *        wait-barrier scoreboard encoding every instruction on sm_5x+ carries. This underlies
 *        both directions of the assembler's control-code handling (decode() for disassembly text,
 *        encode() for reading it back), and until now had no test anywhere referencing it by name.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    // mergeCode/splitCode round trip: waitbar bit {2} only, readbar=0, writebar=1, no yield, stall=7.
    // This is the exact worked example from CuControlCode.hpp's own doc comment ("B--2---:R0:W1:-:S07"),
    // used here as independently-verifiable ground truth rather than a value derived from the code
    // under test itself.
    const std::uint32_t code = CuControlCode::mergeCode(/*waitbar=*/0b000100, /*readbar=*/0, /*writebar=*/1,
                                                          /*yieldFlag=*/1, /*stall=*/7);
    std::uint32_t wb = 0, rb = 0, wrb = 0, yf = 0, st = 0;
    CuControlCode::splitCode(code, wb, rb, wrb, yf, st);
    t.check("splitCode recovers exactly the fields mergeCode packed",
            wb == 0b000100 && rb == 0 && wrb == 1 && yf == 1 && st == 7);

    t.checkEqual("decode() matches the documented worked example exactly", CuControlCode::decode(code),
                 std::string("B--2---:R0:W1:-:S07"));
    t.checkEqual("encode() is decode()'s exact inverse", CuControlCode::encode("B--2---:R0:W1:-:S07"), code);

    // splitCode2: same fields, but yield+stall combined into one 5-bit "ystall" (yield in bit 4).
    std::uint32_t wb2 = 0, rb2 = 0, wrb2 = 0, ystall = 0;
    CuControlCode::splitCode2(code, wb2, rb2, wrb2, ystall);
    t.check("splitCode2 matches splitCode's waitbar/readbar/writebar and packs yield+stall into one field",
            wb2 == wb && rb2 == rb && wrb2 == wrb && ystall == ((yf << 4) | st));

    // CuControlCode accessors, built from that same code.
    const CuControlCode cc(code);
    t.check("accessors expose the same fields splitCode extracted",
            !cc.isYield() /* yieldFlag=1 means NOT yielding */ && cc.getStallCount() == 7 && cc.getReadSB() == 0 &&
                cc.getWriteSB() == 1 && cc.getBarrierSet() == std::set<int>{2});

    // Sentinel values: readbar/writebar == 7 mean "unset" (-1), and every waitbar bit clear means
    // an empty barrier set; yieldFlag == 0 means "is yielding".
    const std::uint32_t unsetCode = CuControlCode::mergeCode(/*waitbar=*/0, /*readbar=*/7, /*writebar=*/7,
                                                               /*yieldFlag=*/0, /*stall=*/0);
    const CuControlCode unsetCc(unsetCode);
    t.check("readbar/writebar==7 decode to -1 (unset), an all-clear waitbar decodes to an empty set, "
            "and yieldFlag==0 means isYield()",
            unsetCc.getReadSB() == -1 && unsetCc.getWriteSB() == -1 && unsetCc.getBarrierSet().empty() && unsetCc.isYield());
    t.checkEqual("decode() renders the all-unset case with every field as '-'", CuControlCode::decode(unsetCode),
                 std::string("B------:R-:W-:Y:S00"));

    // Every waitbar bit set.
    const std::uint32_t allWaitCode = CuControlCode::mergeCode(/*waitbar=*/0b111111, 0, 0, 1, 0);
    t.check("every waitbar bit set decodes to the full barrier set {0..5} and renders each digit",
            CuControlCode(allWaitCode).getBarrierSet() == std::set<int>{0, 1, 2, 3, 4, 5} &&
                CuControlCode::decode(allWaitCode).substr(0, 7) == "B012345");

    // matchesControlCodePattern / encode()'s rejection of malformed input.
    t.check("matchesControlCodePattern accepts a well-formed string and rejects garbage",
            CuAsm::matchesControlCodePattern("B--2---:R0:W1:-:S07") && !CuAsm::matchesControlCodePattern("not a control code"));
    t.checkThrows<std::invalid_argument>("encode() throws std::invalid_argument on malformed input",
                                          [] { (void)CuControlCode::encode("garbage"); });

    // Round-tripping decode()->encode() for a value with a maximal stall count (4 bits: 0-15).
    const std::uint32_t maxStallCode = CuControlCode::mergeCode(0, 3, 2, 0, 15);
    t.checkEqual("decode()/encode() round-trips a maximal stall count",
                 CuControlCode::encode(CuControlCode::decode(maxStallCode)), maxStallCode);

    return t.finish("test_CuControlCode");
}
