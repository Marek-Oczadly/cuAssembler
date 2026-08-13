#include "CuSMVersion.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "CuNVInfo.hpp"

namespace CuAsm {

namespace {

// ---- SM version validation, mirroring makeVersionDict()/SMVersionDict ----
//
// Limited to the versions with a shipped DefaultInsAsmRepos (CuAsm/InsAsmRepos/) plus the
// versions in InsAsmReposAliasDict that fall back to one of those (62->61, 72->75, 87->86) and
// so are supported without issue. Versions with neither (e.g. 35, 37, 50, 52, 53, 89, 90) are
// rejected up front rather than accepted and later failing at repos-load time with an empty,
// non-functional repos (see CuInsAssemblerRepos::setToDefaultInsAsmDict).
const std::set<int>& validSMVersions() {
    static const std::set<int> versions = {60, 61, 62, 70, 72, 75, 80, 86, 87};
    return versions;
}

int validateVersionNumber(int version) {
    if (!validSMVersions().count(version)) {
        throw std::invalid_argument("Invalid SM version " + std::to_string(version) + "!!!");
    }
    return version;
}

int validateVersionString(const std::string& version) {
    std::string upper = version;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                    [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    for (int v : validSMVersions()) {
        const std::string vs = std::to_string(v);
        if (upper == "SM" + vs || upper == "SM_" + vs || upper == vs) {
            return v;
        }
    }

    throw std::invalid_argument("Invalid SM version " + version + "!!!");
}

// ---- Control-code bit layout constants ----

constexpr std::uint64_t kCCMask0_5x6x = 0x000000000001ffffull;
constexpr std::uint64_t kCCMask1_5x6x = 0x0000003fffe00000ull;
constexpr std::uint64_t kCCMask2_5x6x = 0x07fffc0000000000ull;

constexpr std::uint64_t kCCReuse0_5x6x = 0x00000000001e0000ull;
constexpr std::uint64_t kCCReuse1_5x6x = 0x000003c000000000ull;
constexpr std::uint64_t kCCReuse2_5x6x = 0x7800000000000000ull;

constexpr int kCCPos0_5x6x = 0;
constexpr int kCCPos1_5x6x = 21;
constexpr int kCCPos2_5x6x = 42;

constexpr int kCCPos_7x8x = 64 + 41;

constexpr std::uint64_t kPredCode5x6x = 0xf0000ull;
constexpr std::uint64_t kPredCode7x8x = 0xf000ull;

constexpr std::uint64_t kPadCCode5x6x = 0x7e0ull;               // [----:B------:R-:W-:Y:S00]
constexpr std::uint64_t kPadICode5x6x = 0x50b0000000070f00ull;  // NOP
constexpr std::uint64_t kPadCCode7x8x = 0x7e0ull;                // [----:B------:R-:W-:Y:S00]
constexpr std::uint64_t kPadICode7x8x = 0x7918ull;               // NOP

const BigInt& b64Mask() {
    static const BigInt mask = (BigInt(1) << 64) - 1;
    return mask;
}

const BigInt& ccMask7x8x() {
    static const BigInt mask = BigInt(0x1ffff) << kCCPos_7x8x;
    return mask;
}

std::span<const std::byte> padBytes5x6x() {
    static constexpr std::uint8_t raw[] = {
        0xe0, 0x07, 0x00, 0xfc, 0x00, 0x80, 0x1f, 0x00,
        0x00, 0x0f, 0x07, 0x00, 0x00, 0x00, 0xb0, 0x50,
        0x00, 0x0f, 0x07, 0x00, 0x00, 0x00, 0xb0, 0x50,
        0x00, 0x0f, 0x07, 0x00, 0x00, 0x00, 0xb0, 0x50,
    };
    return std::as_bytes(std::span(raw));
}

std::span<const std::byte> padBytes7x8x() {
    static constexpr std::uint8_t raw[] = {
        0x18, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x0f, 0x00,
    };
    return std::as_bytes(std::span(raw));
}

// ---- Byte <-> BigInt helpers ----

BigInt readLittleEndianBigInt(std::span<const std::byte> bytes, std::size_t pos, std::size_t n) {
    BigInt val = 0;
    for (std::size_t i = 0; i < n; ++i) {
        val |= BigInt(std::to_integer<unsigned char>(bytes[pos + i])) << (8 * i);
    }
    return val;
}

void appendLittleEndian(std::vector<std::byte>& out, const BigInt& value, int nbytes) {
    static const BigInt byteMask(0xff);
    BigInt v = value;
    for (int i = 0; i < nbytes; ++i) {
        out.push_back(static_cast<std::byte>((v & byteMask).convert_to<unsigned int>()));
        v >>= 8;
    }
}

// ---- Control-code remix for sm5x/6x, mirroring CuSMVersion.remixCode_5x_6x ----

struct RemixResult5x6x {
    BigInt cc;
    BigInt mi0;
    BigInt mi1;
    BigInt mi2;
};

RemixResult5x6x remixCode5x6x(const BigInt& i0, const BigInt& i1, const BigInt& i2,
                                const BigInt& c0, const BigInt& c1, const BigInt& c2) {
    const BigInt mc0 = c0 + (i0 >> 64);
    const BigInt mc1 = c1 + (i1 >> 64);
    const BigInt mc2 = c2 + (i2 >> 64);

    BigInt cc = mc0 << kCCPos0_5x6x;
    cc += mc1 << kCCPos1_5x6x;
    cc += mc2 << kCCPos2_5x6x;

    return {cc, i0 & b64Mask(), i1 & b64Mask(), i2 & b64Mask()};
}

std::vector<std::byte> mergeCtrlCodes5x6x(const std::vector<BigInt>& insCodeList, const std::vector<std::uint32_t>& ctrlCodeList) {
    const std::size_t nIns = insCodeList.size();
    if (ctrlCodeList.size() != nIns) {
        throw std::runtime_error("Length of control codes(" + std::to_string(ctrlCodeList.size()) +
                                  ") != length of instruction(" + std::to_string(nIns) + ")!");
    }

    std::vector<std::byte> out;
    const std::size_t nccodeIntact = nIns / 3; // intact part of control code groups (1+3)
    for (std::size_t i = 0; i < nccodeIntact; ++i) {
        const RemixResult5x6x r = remixCode5x6x(insCodeList[3 * i], insCodeList[3 * i + 1], insCodeList[3 * i + 2],
                                                  BigInt(ctrlCodeList[3 * i]), BigInt(ctrlCodeList[3 * i + 1]),
                                                  BigInt(ctrlCodeList[3 * i + 2]));
        appendLittleEndian(out, r.cc, 8);
        appendLittleEndian(out, r.mi0, 8);
        appendLittleEndian(out, r.mi1, 8);
        appendLittleEndian(out, r.mi2, 8);
    }

    if (nccodeIntact * 3 != nIns) {
        const std::size_t ntail = nIns - nccodeIntact * 3;

        std::vector<BigInt> tIns(insCodeList.begin() + static_cast<std::ptrdiff_t>(3 * nccodeIntact), insCodeList.end());
        std::vector<BigInt> tCtrl;
        for (std::size_t i = 3 * nccodeIntact; i < nIns; ++i) {
            tCtrl.emplace_back(ctrlCodeList[i]);
        }

        const std::size_t npad = 3 - ntail;
        for (std::size_t i = 0; i < npad; ++i) {
            tCtrl.emplace_back(kPadCCode5x6x);
            tIns.emplace_back(kPadICode5x6x);
        }

        const RemixResult5x6x r = remixCode5x6x(tIns[0], tIns[1], tIns[2], tCtrl[0], tCtrl[1], tCtrl[2]);
        appendLittleEndian(out, r.cc, 8);
        appendLittleEndian(out, r.mi0, 8);
        appendLittleEndian(out, r.mi1, 8);
        appendLittleEndian(out, r.mi2, 8);
    }

    return out;
}

std::vector<std::byte> mergeCtrlCodes7x8x(const std::vector<BigInt>& insCodeList, const std::vector<std::uint32_t>& ctrlCodeList) {
    const std::size_t nIns = insCodeList.size();
    if (ctrlCodeList.size() != nIns) {
        throw std::runtime_error("Length of control codes(" + std::to_string(ctrlCodeList.size()) +
                                  ") != length of instruction(" + std::to_string(nIns) + ")!");
    }

    std::vector<std::byte> out;
    out.reserve(nIns * 16);
    for (std::size_t i = 0; i < nIns; ++i) {
        const BigInt code = (BigInt(ctrlCodeList[i]) << kCCPos_7x8x) + insCodeList[i];
        appendLittleEndian(out, code, 16);
    }

    return out;
}

// ---- QNAN disassembly hacks, mirroring CuSMVersion.hackDisassembly_5x_6x/_7x_8x ----

const std::regex& qnanPattern() {
    static const std::regex re(R"((\+|-)QNAN\b)");
    return re;
}

std::string hackDisassembly7x8x(std::uint64_t code, const std::string& asmLine) {
    if (!std::regex_search(asmLine, qnanPattern())) {
        return asmLine;
    }

    const std::uint64_t fimm = (code & (std::uint64_t(0xffffffffu) << 32)) >> 32;
    const std::string rep = std::format("0f{:08x}", static_cast<unsigned int>(fimm));
    return std::regex_replace(asmLine, qnanPattern(), rep);
}

std::string hackDisassembly5x6x(std::uint64_t code, const std::string& asmLine) {
    if (!std::regex_search(asmLine, qnanPattern())) {
        return asmLine;
    }

    constexpr int blen = 32;
    const std::uint64_t bmask = (std::uint64_t(1) << blen) - 1;
    const std::uint64_t fimm = (code & (bmask << 20)) >> 20;

    const bool neg = asmLine.find("-QNAN") != std::string::npos;
    const std::string rep = std::format("{}0f{:05x}", neg ? "-" : "", static_cast<unsigned int>(fimm));
    return std::regex_replace(asmLine, qnanPattern(), rep); // FIXME: neg sign moved? (mirrors python original)
}

// ---- Float immediate packing, mirroring CuSMVersion.convertFloatImme's struct.pack/unpack ----

std::string toLowerTrim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }

    std::string out = s.substr(begin, end - begin);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Converts a (already float32-rounded) IEEE754 binary32 bit pattern to its binary16 (half)
/// equivalent, round-to-nearest-even, mirroring struct.pack('e', ...).
std::uint16_t floatBitsToHalfBits(std::uint32_t bits) {
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t rawExp = (bits >> 23) & 0xffu;
    std::uint32_t mantissa = bits & 0x7fffffu;

    if (rawExp == 0xffu) { // Inf / NaN
        if (mantissa != 0) {
            return static_cast<std::uint16_t>(sign | 0x7e00u); // quiet NaN
        }
        return static_cast<std::uint16_t>(sign | 0x7c00u); // Inf
    }

    std::int32_t exponent = static_cast<std::int32_t>(rawExp) - 127 + 15;

    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<std::uint16_t>(sign); // underflow to zero
        }

        mantissa |= 0x800000u; // implicit leading 1
        const std::int32_t shift = 14 - exponent;
        std::uint32_t halfMantissa = mantissa >> shift;
        const std::uint32_t remainderMask = (1u << shift) - 1u;
        const std::uint32_t remainder = mantissa & remainderMask;
        const std::uint32_t halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (halfMantissa & 1u) != 0)) {
            ++halfMantissa;
        }
        return static_cast<std::uint16_t>(sign | halfMantissa);
    }

    if (exponent >= 0x1f) {
        return static_cast<std::uint16_t>(sign | 0x7c00u); // overflow to Inf
    }

    std::uint32_t halfMantissa = mantissa >> 13;
    const std::uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (halfMantissa & 1u) != 0)) {
        ++halfMantissa;
        if (halfMantissa == 0x400u) {
            halfMantissa = 0;
            ++exponent;
            if (exponent >= 0x1f) {
                return static_cast<std::uint16_t>(sign | 0x7c00u);
            }
        }
    }

    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10) | halfMantissa);
}

/// @note python packs the half directly from its unbounded-precision double via struct's own
///       double->half rounding. Here the double is first rounded to float32 (a single hardware
///       IEEE754 round-to-nearest-even step), then that float32 is converted to half via the
///       bit-exact routine above; this can theoretically differ from a direct double->half
///       conversion by 1 ULP in extremely rare double-rounding cases, which is an acceptable
///       simplification since C++ has no built-in binary16 type to convert through directly.
std::uint16_t doubleToHalfBits(double d) {
    const float f = static_cast<float>(d);
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return floatBitsToHalfBits(bits);
}

std::uint64_t packFloatBits(double fv, char prec) {
    switch (prec) {
        case 'h':
        case 'H':
            return doubleToHalfBits(fv);
        case 'f':
        case 'F': {
            const float f = static_cast<float>(fv);
            std::uint32_t bits;
            std::memcpy(&bits, &f, sizeof(bits));
            return bits;
        }
        case 'd':
        case 'D': {
            std::uint64_t bits;
            std::memcpy(&bits, &fv, sizeof(bits));
            return bits;
        }
        default:
            throw std::invalid_argument(std::string("Unknown float precision '") + prec + "'!");
    }
}

/// Returns (fullbits, keepbits) for a given precision, mirroring
/// CuSMVersion.FloatImmeFormat_5x_6x/FloatImmeFormat_7x_8x (the ifmt/ofmt struct format chars are
/// implicit in packFloatBits() above, so only the two integers are needed here).
std::pair<int, int> floatImmeFormat(bool is5x6x, char prec) {
    switch (prec) {
        case 'h':
        case 'H':
            return {16, 16};
        case 'f':
        case 'F':
            return is5x6x ? std::pair{32, 20} : std::pair{32, 32};
        case 'd':
        case 'D':
            return is5x6x ? std::pair{64, 20} : std::pair{64, 32};
        default:
            throw std::invalid_argument(std::string("Unknown float precision '") + prec + "'!");
    }
}

} // namespace

const std::map<int, int> CuSMVersion::InsAsmReposAliasDict = {
    {62, 61},
    {72, 75},
    {87, 86},
};

CuSMVersion::CuSMVersion(int version)
    : m_Version(validateVersionNumber(version)), m_Major(m_Version / 10), m_Minor(m_Version % 10) {
}

CuSMVersion::CuSMVersion(const std::string& version)
    : m_Version(validateVersionString(version)), m_Major(m_Version / 10), m_Minor(m_Version % 10) {
}

int CuSMVersion::getMajor() const {
    return m_Major;
}

int CuSMVersion::getMinor() const {
    return m_Minor;
}

int CuSMVersion::getVersionNumber() const {
    return m_Version;
}

std::string CuSMVersion::getVersionString() const {
    return "SM_" + std::to_string(m_Version);
}

std::uint64_t CuSMVersion::getNOP() const {
    return (m_Major <= 6) ? kPadICode5x6x : kPadICode7x8x;
}

std::span<const std::byte> CuSMVersion::getPadBytes() const {
    return (m_Major <= 6) ? padBytes5x6x() : padBytes7x8x();
}

int CuSMVersion::getInstructionLength() const {
    return (m_Major <= 6) ? 8 : 16;
}

std::uint64_t CuSMVersion::getInsOffsetFromIndex(int idx) const {
    if (m_Major <= 6) {
        return static_cast<std::uint64_t>((idx / 3 + 1) * 8 + idx * 8);
    }
    return static_cast<std::uint64_t>(idx) * 16;
}

int CuSMVersion::getInsIndexFromOffset(std::uint64_t offset) const {
    if (m_Major <= 6) {
        const std::uint64_t ridx = offset >> 3;
        if ((ridx & 0x3) == 0) {
            return -1;
        }
        return static_cast<int>((ridx >> 2) * 3 + (ridx & 0x3) - 1);
    }
    return static_cast<int>(offset >> 4);
}

std::uint64_t CuSMVersion::getNextInsAddr(std::uint64_t addr) const {
    const int idx = getInsIndexFromOffset(addr);
    return getInsOffsetFromIndex(idx + 1);
}

std::uint64_t CuSMVersion::getPrevInsAddr(std::uint64_t addr) const {
    const int idx = getInsIndexFromOffset(addr);
    return getInsOffsetFromIndex(idx - 1);
}

std::string CuSMVersion::getInsRelocationType(const std::string& key) const {
    static const std::map<std::string, std::string> relMaps5x6x = {
        {"32@hi", "R_CUDA_ABS32_HI_20"},
        {"32@lo", "R_CUDA_ABS32_LO_20"},
        {"target", "R_CUDA_ABS32_20"},
    };
    static const std::map<std::string, std::string> relMaps7x8x = {
        {"32@hi", "R_CUDA_ABS32_HI_32"},
        {"32@lo", "R_CUDA_ABS32_LO_32"},
        {"target", "R_CUDA_ABS47_34"},
    };

    const std::map<std::string, std::string>& table = (m_Major <= 6) ? relMaps5x6x : relMaps7x8x;
    return table.at(key);
}

int CuSMVersion::getTextSectionSizeUnit() const {
    return (m_Major <= 6) ? 64 : 128;
}

bool CuSMVersion::setRegCountInNVInfo(CuNVInfo& nvinfo, const std::map<int, int>& regCountDict) const {
    return nvinfo.setRegCount(regCountDict);
}

bool CuSMVersion::needsDescHack() const {
    return m_Major >= 8;
}

std::string CuSMVersion::hackDisassembly(std::uint64_t code, const std::string& asmLine) const {
    return (m_Major <= 6) ? hackDisassembly5x6x(code, asmLine) : hackDisassembly7x8x(code, asmLine);
}

std::pair<std::vector<std::uint32_t>, std::vector<BigInt>>
CuSMVersion::splitCtrlCodeFromBytes(std::span<const std::byte> codebytes) const {
    if (m_Major <= 6) {
        if ((codebytes.size() & 0x1f) != 0) {
            throw std::invalid_argument("splitCtrlCodeFromBytes: length must be a multiple of 32 bytes for sm5x/6x!");
        }

        std::vector<BigInt> intList;
        intList.reserve(codebytes.size() / 8);
        for (std::size_t pos = 0; pos + 8 <= codebytes.size(); pos += 8) {
            intList.push_back(readLittleEndianBigInt(codebytes, pos, 8));
        }
        return splitCtrlCodeFromIntList_5x_6x(intList);
    }

    if ((codebytes.size() & 0xf) != 0) {
        throw std::invalid_argument("splitCtrlCodeFromBytes: length must be a multiple of 16 bytes for sm7x/8x!");
    }

    std::vector<BigInt> intList;
    intList.reserve(codebytes.size() / 16);
    for (std::size_t pos = 0; pos + 16 <= codebytes.size(); pos += 16) {
        intList.push_back(readLittleEndianBigInt(codebytes, pos, 16));
    }
    return splitCtrlCodeFromIntList_7x_8x(intList);
}

std::vector<std::byte> CuSMVersion::mergeCtrlCodes(const std::vector<BigInt>& insCodeList,
                                                    const std::vector<std::uint32_t>& ctrlCodeList) const {
    return (m_Major <= 6) ? mergeCtrlCodes5x6x(insCodeList, ctrlCodeList) : mergeCtrlCodes7x8x(insCodeList, ctrlCodeList);
}

std::pair<std::vector<std::uint32_t>, std::vector<BigInt>>
CuSMVersion::splitCtrlCodeFromIntList_5x_6x(const std::vector<BigInt>& intList) {
    if ((intList.size() & 0x3) != 0) {
        throw std::invalid_argument("splitCtrlCodeFromIntList_5x_6x: length must be a multiple of 4!");
    }

    std::vector<std::uint32_t> ctrlCodeList;
    std::vector<BigInt> insCodeList;
    ctrlCodeList.reserve(intList.size() / 4 * 3);
    insCodeList.reserve(intList.size() / 4 * 3);

    for (std::size_t i = 0; i < intList.size(); i += 4) {
        const BigInt& ccode = intList[i];

        const BigInt cc0 = (ccode & BigInt(kCCMask0_5x6x)) >> kCCPos0_5x6x;
        const BigInt cc1 = (ccode & BigInt(kCCMask1_5x6x)) >> kCCPos1_5x6x;
        const BigInt cc2 = (ccode & BigInt(kCCMask2_5x6x)) >> kCCPos2_5x6x;

        // Reuse-cache bits are folded onto each instruction code above bit 64, mirroring the
        // python original's unbounded ints; BigInt keeps them losslessly.
        BigInt c0 = intList[i + 1] + (((ccode & BigInt(kCCReuse0_5x6x)) >> kCCPos0_5x6x) << 64);
        BigInt c1 = intList[i + 2] + (((ccode & BigInt(kCCReuse1_5x6x)) >> kCCPos1_5x6x) << 64);
        BigInt c2 = intList[i + 3] + (((ccode & BigInt(kCCReuse2_5x6x)) >> kCCPos2_5x6x) << 64);

        ctrlCodeList.push_back(cc0.convert_to<std::uint32_t>());
        ctrlCodeList.push_back(cc1.convert_to<std::uint32_t>());
        ctrlCodeList.push_back(cc2.convert_to<std::uint32_t>());

        insCodeList.push_back(std::move(c0));
        insCodeList.push_back(std::move(c1));
        insCodeList.push_back(std::move(c2));
    }

    return {ctrlCodeList, insCodeList};
}

std::pair<std::vector<std::uint32_t>, std::vector<BigInt>>
CuSMVersion::splitCtrlCodeFromIntList_7x_8x(const std::vector<BigInt>& intList) {
    std::vector<std::uint32_t> ctrlCodeList;
    std::vector<BigInt> insCodeList;
    ctrlCodeList.reserve(intList.size());
    insCodeList.reserve(intList.size());

    for (const BigInt& c : intList) {
        const BigInt cc = c & ccMask7x8x();
        BigInt ic = c ^ cc;

        ctrlCodeList.push_back((cc >> kCCPos_7x8x).convert_to<std::uint32_t>());
        insCodeList.push_back(std::move(ic));
    }

    return {ctrlCodeList, insCodeList};
}

std::string CuSMVersion::formatCode(const BigInt& code) const {
    const int width = (m_Major <= 6) ? 22 : 32;
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setfill('0') << std::setw(width) << code;
    return oss.str();
}

const std::set<std::string>& CuSMVersion::getPosDepOpcodes() const {
    static const std::set<std::string> common = {
        "I2I", "F2F", "IDP", "TLD4", "VADD", "VMAD", "VSHL", "VSHR",
        "VSET", "VSETP", "VMNMX", "VABSDIFF", "VABSDIFF4",
    };
    static const std::set<std::string> sm5x6x = [] {
        std::set<std::string> s = common;
        s.insert({"XMAD", "IMAD", "IMAD32I", "IMADSP", "IMUL", "IMUL32I", "PSET", "PSETP"});
        return s;
    }();
    static const std::set<std::string> sm7x = [] {
        std::set<std::string> s = common;
        s.insert({"HMMA", "IMMA"});
        return s;
    }();
    static const std::set<std::string> sm8x = [] {
        std::set<std::string> s = common;
        s.insert({"HMMA", "IMMA", "I2IP", "F2FP"});
        return s;
    }();

    if (m_Major <= 6) {
        return sm5x6x;
    }
    if (m_Major == 7) {
        return sm7x;
    }
    if (m_Major == 8) {
        return sm8x;
    }
    return common;
}

const std::set<std::string>& CuSMVersion::getNVInfoAttrAutoGenSet() const {
    static const std::set<std::string> autoGen7x8x = {
        "EIATTR_CTAIDZ_USED",
        "EIATTR_WMMA_USED",
        "EIATTR_EXIT_INSTR_OFFSETS",
    };
    static const std::set<std::string> autoGen5x6x = {
        "EIATTR_CTAIDZ_USED",
        "EIATTR_WMMA_USED",
        "EIATTR_EXIT_INSTR_OFFSETS",
        "EIATTR_S2RCTAID_INSTR_OFFSETS",
    };

    return (m_Major <= 6) ? autoGen5x6x : autoGen7x8x;
}

const std::set<std::string>& CuSMVersion::getNVInfoAttrManualGenSet() const {
    static const std::set<std::string> manualGen5x6x = {};
    static const std::set<std::string> manualGen7x8x = {"EIATTR_COOP_GROUP_INSTR_OFFSETS"};

    return (m_Major <= 6) ? manualGen5x6x : manualGen7x8x;
}

std::pair<std::uint64_t, std::vector<std::string>> CuSMVersion::convertFloatImme(const std::string& fval, char prec, int nbits) const {
    std::string val = toLowerTrim(fval);

    std::vector<std::string> modi;
    if (m_Major <= 6) {
        if (!val.empty() && val.front() == '-') {
            val = val.substr(1);
            modi.push_back("FINeg");
        } else if (val.size() >= 4 && val.compare(val.size() - 4, 4, ".neg") == 0) { // Only for maxwell/pascal?
            val = val.substr(0, val.size() - 4);
            modi.push_back("ExplicitFINeg");
        }
    }

    if (val.size() >= 2 && val.compare(0, 2, "0f") == 0) {
        const std::uint64_t v = std::stoull(val.substr(2), nullptr, 16);
        return {v, modi};
    }

    const double fv = std::stod(val);
    const auto [fullBits, keepBits] = floatImmeFormat(m_Major <= 6, prec);
    std::uint64_t ival = packFloatBits(fv, prec);

    const int truncBits = fullBits - std::max(nbits, keepBits);
    if (truncBits > 0) {
        ival >>= truncBits;
    }

    return {ival, modi};
}

std::pair<std::int64_t, std::vector<std::string>> CuSMVersion::splitIntImmeModifier(const std::string& insOp, std::int64_t intVal) const {
    if (m_Major <= 6 && !insOp.ends_with("32I") && (intVal & 0x80000) != 0) {
        const std::int64_t newVal = intVal - (intVal & 0x80000);
        return {newVal, {"ImplicitNegIntImme"}};
    }
    return {intVal, {}};
}

bool CuSMVersion::operator==(const CuSMVersion& other) const {
    return m_Version == other.m_Version;
}

bool CuSMVersion::operator!=(const CuSMVersion& other) const {
    return m_Version != other.m_Version;
}

std::optional<std::tuple<std::uint64_t, BigInt, std::string>>
CuSMVersion::genPredCode(std::uint64_t addr, const BigInt& code, const std::string& asmText) const {
    if (!asmText.empty() && asmText.front() == '@') {
        return std::nullopt;
    }

    // CHECK: currently seems all uniform path opcodes with a uniform predicate start with U
    std::string s2;
    if (!asmText.empty() && asmText.front() == 'U') {
        if (asmText.rfind("UNDEF", 0) == 0) { // UNDEF is reserved for un-disassembled instructions
            return std::nullopt;
        }
        s2 = "@UP0 " + asmText;
    } else {
        s2 = "@P0 " + asmText;
    }

    const BigInt pred = (m_Major <= 6) ? BigInt(kPredCode5x6x) : BigInt(kPredCode7x8x);
    // @P0 will set the predicate bit to zero
    const BigInt code2 = code ^ (code & pred);

    return std::make_tuple(addr, code2, s2);
}

} // namespace CuAsm
