#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#include "../CuAsm/CuInsFeeder.hpp"
#include "utils/TestUtilsCommon.hpp"

using CuAsm::BigInt;
using CuAsm::CuInsFeeder;
using CuAsm::CuInsRecord;

namespace {

/** @brief Formats v as a lowercase, zero-padded 4-hex-digit address, e.g. "0010". */
std::string hex4(std::uint64_t v) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(4) << v;
    return oss.str();
}

/** @brief Formats v as a lowercase, zero-padded 16-hex-digit code word, e.g. "0x00000a0000017a02". */
std::string hex16(std::uint64_t v) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setfill('0') << std::setw(16) << v;
    return oss.str();
}

/**
 * @brief Builds one sm_7x/8x-style two-line instruction record, matching the exact format
 *        CuInsFeeder's insCodeRe/codeOnlyRe regexes expect (see CuInsFeeder.cpp): an addr+asm+low
 *        line, then a second line carrying the high 64 bits of the 128-bit instruction code. Using
 *        high=0 keeps the expected decoded ctrl code and instruction code trivial to hand-verify:
 *        with the whole high word zero, none of the 17 control-code bits (which all live in the
 *        high word - see CuSMVersion::kCCPos_7x8x/ccMask7x8x) are set, so ctrl decodes to exactly 0
 *        and the decoded instruction code is exactly the low word, unchanged.
 * @param addr Instruction address (goes in the leading addr comment).
 * @param asmBody Disassembly text, without the trailing " ;" (added here).
 * @param low Low 64 bits of the instruction code (the "real" bits, since high=0).
 * @return The two fixture lines, newline-terminated.
 **/
std::string buildIns(std::uint64_t addr, const std::string& asmBody, std::uint64_t low) {
    std::ostringstream oss;
    oss << "        /*" << hex4(addr) << "*/ " << asmBody << " ; /* " << hex16(low) << " */\n";
    oss << "                                                        /* " << hex16(0) << " */\n";
    return oss.str();
}

/** @brief A minimal but complete two-instruction sm_75 sass-dump-format fixture. */
std::string buildFixture() {
    return "Function : testKernel\n"
           ".headerflags    @\"EF_CUDA_SM75 EF_CUDA_TEXMODE_UNIFIED\"\n" +
           buildIns(0x0000, "MOV R1, c[0x0][0x28]", 0x00000a0000017a02ULL) + buildIns(0x0010, "EXIT", 0x000000000000794dULL);
}

} // namespace

/**
 * @brief Exercises CuAsm::CuInsFeeder against hand-built sm_75 (Sm7x8x-family) sass-dump-format
 *        fixtures fed through an istringstream - no nvcc/nvdisasm/cuobjdump subprocess needed,
 *        since the class itself only ever consumes an already-produced text stream. Every fixture
 *        line here was checked by hand against CuInsFeeder.cpp's line-format regexes
 *        (insCodeRe/codeOnlyRe/funcNameRe/headerFlagRe) and its Sm7x8x parser state machine before
 *        being written, and the expected decoded values were derived independently (by choosing
 *        an all-zero high code word, so the control-code split is trivial to verify by hand)
 *        rather than by calling the code under test on itself.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    // ---- basic feed: FuncName/HeaderFlag tracking, and both instructions decoded correctly ----
    {
        std::istringstream ss(buildFixture());
        CuInsFeeder feeder(ss);

        const std::optional<CuInsRecord> rec1 = feeder.next();
        t.check("the first record has the right addr/asmText/code/ctrl, and CurrFuncName/CurrArch "
                "were updated from the Function/.headerflags lines",
                rec1.has_value() && rec1->addr == 0 && rec1->asmText == "MOV R1, c[0x0][0x28] ;" &&
                    rec1->code == BigInt("0x00000a0000017a02") && rec1->ctrl == 0 && feeder.CurrFuncName == "testKernel" &&
                    feeder.CurrArch == "SM_75");

        const std::optional<CuInsRecord> rec2 = feeder.next();
        t.check("the second record (an all-zero high code word) decodes with ctrl=0 and code "
                "exactly equal to its low word",
                rec2.has_value() && rec2->addr == 0x10 && rec2->asmText == "EXIT ;" &&
                    rec2->code == BigInt("0x000000000000794d") && rec2->ctrl == 0);

        t.check("the feeder is exhausted after both instructions", !feeder.next().has_value());
    }

    // ---- archFilter: a filter that doesn't match the fixture's actual arch yields nothing ----
    {
        std::istringstream ss(buildFixture());
        CuInsFeeder feeder(ss, /*archFilter=*/"sm_61");
        t.check("an archFilter that doesn't match the fixture's sm_75 headerflags line yields no records",
                !feeder.next().has_value());
    }

    // ---- insFilter: only instructions matching the filter regex are yielded ----
    {
        std::istringstream ss(buildFixture());
        CuInsFeeder feeder(ss, /*archFilter=*/"", /*insFilter=*/"EXIT");
        const std::optional<CuInsRecord> rec = feeder.next();
        t.check("an insFilter of \"EXIT\" skips the MOV and yields only the EXIT instruction",
                rec.has_value() && rec->asmText == "EXIT ;" && !feeder.next().has_value());
    }

    // ---- reuse: next() auto-restarts once a prior iteration has run to exhaustion ----
    {
        std::istringstream ss(buildFixture());
        CuInsFeeder feeder(ss);
        (void)feeder.next();                                     // 1st instruction
        (void)feeder.next();                                     // 2nd instruction
        const bool exhausted = !feeder.next().has_value();        // exhausted
        const std::optional<CuInsRecord> restarted = feeder.next(); // should auto-restart
        t.check("calling next() again after exhaustion auto-restarts and re-yields the first record",
                exhausted && restarted.has_value() && restarted->addr == 0 && restarted->asmText == "MOV R1, c[0x0][0x28] ;");
    }

    // ---- trans(): rejects an unrecognized codeOnlyLineMode before ever touching the stream ----
    {
        std::istringstream ss(""); // deliberately empty/invalid - trans() must fail on the mode
        CuInsFeeder feeder(ss);    // check before it would need real content
        std::ostringstream out;
        t.checkThrows<std::invalid_argument>("trans() rejects an unrecognized codeOnlyLineMode",
                                              [&] { feeder.trans(out, "not_keep_or_none"); });
    }

    return t.finish("test_CuInsFeeder");
}
