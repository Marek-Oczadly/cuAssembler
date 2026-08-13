#include "../../CuAsm/utils/BigNum.hpp"
#include "TestUtilsCommon.hpp"

using CuAsm::BigInt;
using CuAsm::BigRational;

/**
 * @brief Exercises CuAsm::BigInt/BigRational (Boost.Multiprecision cpp_int/cpp_rational aliases)
 *        the way the rest of CuAsm actually relies on them: shifts and masks well past the
 *        64-bit boundary (instruction codes are up to 128 bits wide, plus a reuse-cache nibble
 *        above that on sm_5x/6x - see CuSMVersion::splitCtrlCodeFromIntList_5x_6x), truncating
 *        conversion back to a fixed-width type, and exact-fraction reduction (used throughout
 *        RationalMatrix's Gauss-Jordan elimination). None of this is testing Boost itself; it's
 *        pinning down that these aliases behave the way CuAsm's own arithmetic assumes.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    // A 105-bit-wide value (the real instruction-encoding width on sm_7x/8x/9x) split into a
    // control-code part and a code part by shifting/masking, mirroring splitCtrlCodeFromBytes.
    const BigInt wide = (BigInt(0x1ffff) << 104) | (BigInt(1) << 63) | BigInt(0xdeadbeefu);
    const BigInt mask64 = (BigInt(1) << 64) - 1;
    t.check("a >64-bit value masks/shifts apart into its low-64 and high parts exactly",
            (wide & mask64) == ((BigInt(1) << 63) | BigInt(0xdeadbeefu)) && (wide >> 104) == BigInt(0x1ffff));

    // Negative values must round-trip through a shift exactly, and compare correctly.
    const BigInt neg = BigInt(-12345);
    t.check("a negative BigInt survives a left/right shift round trip and compares < 0", ((neg << 10) >> 10) == neg && neg < 0);

    // Truncating conversion to a fixed-width type after masking, exactly the pattern
    // CuSMVersion::appendLittleEndian uses to emit one byte at a time from a BigInt.
    const BigInt v = BigInt(0x123456789ULL);
    t.check("masking out individual bytes of a BigInt and converting each to unsigned int works",
            static_cast<unsigned int>(v & BigInt(0xff)) == 0x89u && static_cast<unsigned int>((v >> 16) & BigInt(0xff)) == 0x45u);

    // Exact reduction, the property RationalMatrix's solve()/nullspace() lean on throughout.
    const BigRational half = BigRational(2, 4);
    t.check("2/4 reduces to lowest terms 1/2", boost::multiprecision::numerator(half) == BigInt(1) &&
                                                    boost::multiprecision::denominator(half) == BigInt(2));

    t.checkEqual("1/3 + 1/6 reduces exactly to 1/2", BigRational(1, 3) + BigRational(1, 6), BigRational(1, 2));

    const BigRational fromInt(BigInt(7));
    t.checkEqual("a BigRational constructed from a whole BigInt has denominator 1",
                 boost::multiprecision::denominator(fromInt), BigInt(1));

    t.check("distinct rationals compare unequal", BigRational(1, 3) != BigRational(1, 2));

    return t.finish("test_BigNum");
}
