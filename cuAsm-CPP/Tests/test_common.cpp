#include <array>
#include <cstddef>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "../CuAsm/common.hpp"
#include "utils/TestUtilsCommon.hpp"

using CuAsm::BigInt;

namespace {

/** @brief Minimal integral-entried matrix satisfying reprHexMat's MatrixT requirements. */
struct IntMatrix {
    int rows_, cols_;
    std::vector<int> data;
    int rows() const { return rows_; }
    int cols() const { return cols_; }
    int operator()(int r, int c) const { return data[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(c)]; }
};

} // namespace

/**
 * @brief Exercises the pure string/formatting helpers in CuAsm/common.hpp/.cpp: bit/hex
 *        formatting (binstr/hexstr/hexFixedWidth/intList2Str), alignment arithmetic, the
 *        .cuasm-section-splitting scanner, byte-dump formatting (bytes2Asm/stringBytes2Asm),
 *        comment stripping, python-str-method mirrors (trim family, splitChar, replaceAll,
 *        pyStrRepr/pyStrListRepr), and the repr-style stream writers. None of this touches a
 *        subprocess or the filesystem; every case here is hand-verified against the
 *        implementation rather than round-tripped against itself.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    // ---- binstr / hexstr / hexFixedWidth ----

    t.checkEqual("binstr pads to bitlen and groups by width", CuAsm::binstr(BigInt(5), 8, 4, " "), std::string("0000 0101"));
    t.checkEqual("binstr of zero is all-zero padding", CuAsm::binstr(BigInt(0), 4, 4, " "), std::string("0000"));
    t.checkEqual("hexstr pads to bitlen/4 hex digits and groups by width", CuAsm::hexstr(BigInt(0xabcd), 32, 4, " "),
                 std::string("0000 abcd"));
    t.checkEqual("hexFixedWidth pads to the total requested width including the 0x prefix",
                 CuAsm::hexFixedWidth(0xab, 8), std::string("0x0000ab"));
    t.checkEqual("hexFixedWidth never truncates a value wider than the requested minimum",
                 CuAsm::hexFixedWidth(0x1234, 4), std::string("0x1234"));

    // ---- alignTo ----

    t.check("alignTo rounds up to the next multiple and reports the pad added",
            CuAsm::alignTo(5, 4) == std::make_pair(std::uint64_t(8), std::uint64_t(3)) &&
                CuAsm::alignTo(8, 4) == std::make_pair(std::uint64_t(8), std::uint64_t(0)));
    t.check("alignTo treats align 0 or 1 as a no-op", CuAsm::alignTo(5, 0) == std::make_pair(std::uint64_t(5), std::uint64_t(0)) &&
                                                            CuAsm::alignTo(5, 1) == std::make_pair(std::uint64_t(5), std::uint64_t(0)));

    // ---- intList2Str ----

    t.checkEqual("intList2Str formats each value as unpadded hex", CuAsm::intList2Str({1, 2, 255}),
                 std::string("[0x1, 0x2, 0xff]"));
    t.checkEqual("intList2Str with a width zero-pads each value", CuAsm::intList2Str({1, 2, 255}, 4),
                 std::string("[0x0001, 0x0002, 0x00ff]"));
    t.checkEqual("intList2Str of an empty list", CuAsm::intList2Str({}), std::string("[]"));

    // ---- splitAsmSection ----
    {
        const std::vector<std::string> lines = {
            "// header comment",       // 0: $FileHeader
            "some header directive",   // 1
            "// .text.foo",            // 2: comment naming the next section (pulled into .text.foo's range)
            ".section .text.foo, ...", // 3
            "code line 1",             // 4
            ".section .text.bar,",     // 5: no matching comment on the preceding line
            "code line 2",             // 6
        };
        const std::map<std::string, std::pair<int, int>> sections = CuAsm::splitAsmSection(lines);
        t.check("splitAsmSection finds $FileHeader plus every named section, with a preceding "
                "same-name comment line pulled into that section's range",
                sections.size() == 3 && sections.at("$FileHeader") == std::make_pair(0, 2) &&
                    sections.at(".text.foo") == std::make_pair(3, 5) && sections.at(".text.bar") == std::make_pair(5, 7));
    }

    // ---- bytes2Asm / stringBytes2Asm ----
    {
        const std::array<unsigned char, 3> raw = {0x41, 0x42, 0x43};
        const auto bytes = std::as_bytes(std::span(raw));
        t.checkEqual("bytes2Asm formats a single line of .byte directives with an addr comment",
                     CuAsm::bytes2Asm(bytes, 8, 0, "    "), std::string("    /*0000*/ .byte 0x41, 0x42, 0x43\n"));
        t.checkEqual("bytes2Asm wraps at width and honors an address offset",
                     CuAsm::bytes2Asm(bytes, 2, 0x10, ""), std::string("/*0010*/ .byte 0x41, 0x42\n/*0012*/ .byte 0x43\n"));
    }
    {
        const std::array<unsigned char, 3> raw = {0x41, 0x42, 0x00};
        const auto strBytes = std::as_bytes(std::span(raw));
        t.checkEqual("stringBytes2Asm reprs each NUL-terminated string and dumps its bytes",
                     CuAsm::stringBytes2Asm(strBytes, "mystr"),
                     std::string("    // mystr[0] = b'AB\\x00' \n    /*0000*/ .byte 0x41, 0x42, 0x00\n\n"));
    }

    // ---- stripComments: structural checks (comment text/markers gone, code preserved, trimmed) ----

    {
        const std::string stripped = CuAsm::stripComments("   FADD R0, R1 ;    // a trailing note   ");
        t.check("stripComments removes a // comment (and its text) and trims the result",
                stripped.find("//") == std::string::npos && stripped.find("trailing") == std::string::npos &&
                    stripped.front() == 'F' && stripped.back() == ';');
    }
    {
        const std::string stripped = CuAsm::stripComments("MOV R1 /* inline note */, R2 ;");
        t.check("stripComments removes a /* */ comment while keeping the surrounding code",
                stripped.find("/*") == std::string::npos && stripped.find("inline") == std::string::npos &&
                    stripped.find("MOV R1") != std::string::npos && stripped.find("R2 ;") != std::string::npos);
    }
    {
        const std::string stripped = CuAsm::stripComments("BRA `(.L_1) (*\"BRANCH_TARGETS .L_1\"*)");
        t.check("stripComments removes a (* *) bra-target note", stripped.find("BRANCH_TARGETS") == std::string::npos &&
                                                                       stripped.find("BRA") != std::string::npos);
    }

    // ---- trim family ----

    t.checkEqual("trim removes leading and trailing whitespace", CuAsm::trim("  hi  "), std::string("hi"));
    t.checkEqual("rtrim only removes trailing whitespace", CuAsm::rtrim("  hi  "), std::string("  hi"));
    t.checkEqual("trimChars strips the given characters from both ends", CuAsm::trimChars("xxhixx", "x"), std::string("hi"));
    t.checkEqual("ltrimChars strips the given characters from the left end only", CuAsm::ltrimChars("xxhixx", "x"),
                 std::string("hixx"));

    // ---- splitChar / replaceAll ----

    t.check("splitChar preserves empty tokens between consecutive delimiters, mirroring str.split",
            CuAsm::splitChar("a,,b,c", ',') == std::vector<std::string>{"a", "", "b", "c"});
    t.checkEqual("replaceAll replaces every non-overlapping occurrence", CuAsm::replaceAll("aXbXc", "X", "--"),
                 std::string("a--b--c"));
    t.checkEqual("replaceAll with an empty search string is a no-op", CuAsm::replaceAll("abc", "", "X"), std::string("abc"));

    // ---- pyStrRepr / pyStrListRepr ----

    t.checkEqual("pyStrRepr quotes and escapes an embedded single quote", CuAsm::pyStrRepr("it's"), std::string("'it\\'s'"));
    t.checkEqual("pyStrRepr on a string with no special characters", CuAsm::pyStrRepr("plain"), std::string("'plain'"));
    t.checkEqual("pyStrListRepr formats each element via pyStrRepr, comma-joined", CuAsm::pyStrListRepr({"a", "b's"}),
                 std::string("['a', 'b\\'s']"));

    // ---- CalledProcessError ----

    {
        const CuAsm::CalledProcessError err(42, "some output");
        t.check("CalledProcessError exposes its return code and captured output, and is a std::exception",
                err.returnCode() == 42 && err.output() == "some output" && std::string(err.what()).find("42") != std::string::npos);
    }

    // ---- reprList / reprDict / reprHexMat ----

    {
        std::ostringstream os;
        CuAsm::reprList(os, std::vector<int>{1, 2, 3});
        t.checkEqual("reprList joins elements with a comma+newline, bracketed", os.str(), std::string("[1,\n2,\n3]"));
    }
    {
        std::ostringstream os;
        CuAsm::reprDict(os, std::map<int, std::string>{{1, "a"}, {2, "b"}});
        t.checkEqual("reprDict joins key:value pairs in key order, comma+newline, braced", os.str(), std::string("{1:a,\n2:b}"));
    }
    t.checkEqual("reprHexMat right-aligns each column to its widest hex entry",
                 CuAsm::reprHexMat(IntMatrix{2, 2, {0, 255, 16, 4095}}),
                 std::string("Matrix([\n[ 0x0,  0xff],\n[0x10, 0xfff],\n])"));

    return t.finish("test_common");
}
