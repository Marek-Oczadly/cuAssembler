#include "CuInsParser.hpp"

#include <cctype>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

#include "common.hpp"
#include "utils/BigNum.hpp"

namespace CuAsm {

namespace {

// ---------------------------------------------------------------------------------------------
// Small string helpers mirroring python str methods used throughout CuInsParser.py.
// ---------------------------------------------------------------------------------------------

/** @brief Strips leading/trailing whitespace, mirroring python's str.strip(). */
std::string strip(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

/** @brief Strips any of the given chars from both ends, mirroring python's str.strip(chars). */
std::string stripChars(const std::string& s, const std::string& chars) {
    std::size_t begin = 0;
    while (begin < s.size() && chars.find(s[begin]) != std::string::npos) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && chars.find(s[end - 1]) != std::string::npos) {
        --end;
    }
    return s.substr(begin, end - begin);
}

/** @brief Strips any of the given chars from the left end only, mirroring python's str.lstrip(chars). */
std::string lstripChars(const std::string& s, const std::string& chars) {
    std::size_t begin = 0;
    while (begin < s.size() && chars.find(s[begin]) != std::string::npos) {
        ++begin;
    }
    return s.substr(begin);
}

/** @brief Splits on a single-char delimiter, preserving empty tokens, mirroring python's str.split(sep). */
std::vector<std::string> splitChar(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == delim) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

/** @brief Replaces every occurrence of a literal substring, mirroring python's str.replace(old, new). */
std::string replaceAll(const std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return s;
    }
    std::string out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t found = s.find(from, pos);
        if (found == std::string::npos) {
            out += s.substr(pos);
            break;
        }
        out += s.substr(pos, found - pos);
        out += to;
        pos = found + from.size();
    }
    return out;
}

/** @brief Parses a (possibly '-'-prefixed) "0x..." hex literal to a signed integer, mirroring python's int(s, 16). */
std::int64_t parseHexSigned(const std::string& s) {
    const bool neg = !s.empty() && s[0] == '-';
    const std::string mag = neg ? s.substr(1) : s;
    const long long v = std::stoll(mag, nullptr, 16);
    return neg ? -v : v;
}

/** @brief python repr() of a string list, e.g. "['0_MOV']", used by dumpInfo. */
std::string pyStrListRepr(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        out += "'" + v[i] + "'";
        if (i + 1 < v.size()) {
            out += ", ";
        }
    }
    out += "]";
    return out;
}

/** @brief Gets the text before the first '.', or the whole string if there is none. */
std::string firstDotSegment(const std::string& s) {
    const std::size_t pos = s.find('.');
    return pos == std::string::npos ? s : s.substr(0, pos);
}

// ---------------------------------------------------------------------------------------------
// Two of the original patterns rely on lookbehind assertions, which std::regex (ECMAScript
// grammar) does not support. Both are reimplemented as small manual scans instead.
// ---------------------------------------------------------------------------------------------

/** @brief Applies a regex_replace, except where the match is immediately preceded by '.', mirroring
 *         the negative lookbehind in p_ConstTrDict's `r'(?<!\.)\bRZ\b'` entry. */
std::string regexReplaceUnlessPrecededByDot(const std::string& s, const std::regex& re, const std::string& replacement) {
    std::string out;
    std::size_t lastPos = 0;
    auto it = std::sregex_iterator(s.begin(), s.end(), re);
    const auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        const std::smatch& match = *it;
        const std::size_t pos = static_cast<std::size_t>(match.position(0));
        const std::size_t len = static_cast<std::size_t>(match.length(0));
        out += s.substr(lastPos, pos - lastPos);
        if (pos > 0 && s[pos - 1] == '.') {
            out += match.str(0);
        } else {
            out += replacement;
        }
        lastPos = pos + len;
    }
    out += s.substr(lastPos);
    return out;
}

/** @brief Whether c counts as a "word or '?'" character for insignificant-space purposes. */
bool isWordOrQ(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '?';
}

/** @brief Removes single spaces not flanked by word/'?' chars on both sides, mirroring
 *         p_InsignificantSpace (whose three lookaround alternatives collectively mean "remove a
 *         space unless both neighbors are word/'?' chars"). */
std::string removeInsignificantSpaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ' ') {
            const bool beforeWord = i > 0 && isWordOrQ(s[i - 1]);
            const bool afterWord = i + 1 < s.size() && isWordOrQ(s[i + 1]);
            if (beforeWord && afterWord) {
                out += ' ';
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

/** @brief Inserts a '+' before every "-0x" not already preceded by '[' or '+', mirroring the
 *         negative lookbehind in __parseAddress's `re.sub(r'(?<![\[\+])-0x', '+-0x', s)`. */
std::string insertPlusBeforeNegHex(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (std::size_t i = 0; i < s.size();) {
        if (i + 2 < s.size() && s[i] == '-' && s[i + 1] == '0' && s[i + 2] == 'x') {
            const bool precededByBracketOrPlus = !out.empty() && (out.back() == '[' || out.back() == '+');
            if (!precededByBracketOrPlus) {
                out += '+';
            }
            out += "-0x";
            i += 3;
        } else {
            out += s[i];
            ++i;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Regex patterns, mirroring the module-level p_* patterns. Named groups aren't available in
// std::regex, so plain capture groups are used instead (indices noted per pattern). Patterns
// used via python's `.match()` (which anchors at the start regardless of an explicit '^') gain an
// explicit '^' here since std::regex_search does not anchor by default.
// ---------------------------------------------------------------------------------------------

const std::regex& insPatternRe() { // 1=Pred 2=Op 3=Operands
    static const std::regex re(R"(^(@!?U?P\w\s+)?\s*([\w\.\?]+)(.*))");
    return re;
}
const std::regex& sbSetRe() {
    static const std::regex re(R"(\{(\d,)*\d\})");
    return re;
}
const std::regex& rzRe() {
    static const std::regex re(R"(\bRZ\b)");
    return re;
}
const std::regex& urzRe() {
    static const std::regex re(R"(\bURZ\b)");
    return re;
}
const std::regex& ptRe() {
    static const std::regex re(R"(\bPT\b)");
    return re;
}
const std::regex& uptRe() {
    static const std::regex re(R"(\bUPT\b)");
    return re;
}
const std::regex& qnanRe() {
    static const std::regex re(R"(\bQNAN\b)");
    return re;
}
const std::regex& whiteSpaceRe() {
    static const std::regex re(R"(\s+)");
    return re;
}
const std::regex& modifierPatternRe() { // 1=PreModi 2=Main 3=PostModi(full span)
    static const std::regex re(R"(^([~\-\|!]*)(.*?)\|?((\.\w+)*)\|?$)");
    return re;
}
const std::regex& indexedPatternRe() { // 1=Label 2=Index
    static const std::regex re(R"(^\b(R|UR|P|UP|B|SB|SBSET|SR)(\d+)$)");
    return re;
}
const std::regex& fiTypeRe() { // boolean check only; no groups are consulted
    static const std::regex re(R"(^(((-?\d+)(\.\d*)?((e|E)[-+]?\d+)?)|([+-]?INF)|([+-]NAN)|-?(0[fF][0-9a-fA-F]+))((\.[a-zA-Z]\w*)*)$)");
    return re;
}
const std::regex& immeModiRe() { // 1=Value 2=ModiSet(full span)
    static const std::regex re(R"(^(.*?)((\.[a-zA-Z]\w*)*)$)");
    return re;
}
const std::regex& constMemTypeRe() { // 1=Bank 2=Addr
    static const std::regex re(R"(^c\[(0x\w+)\]\[([+-?\w\.]+)\])");
    return re;
}
const std::regex& urConstMemTypeRe() { // 1=URBank 2=Addr
    static const std::regex re(R"(^cx\[(UR\w+)\]\[([+-?\w\.]+)\])");
    return re;
}
const std::regex& descAddressTypeRe() { // 1=URIndex 2=Addr
    static const std::regex re(R"(^desc\[(UR\d+)\](\[.*\])$)");
    return re;
}
const std::regex& rImmeAddrRe() { // 1=R 2=II
    static const std::regex re(R"((R\d+)\s*(-?0x[0-9a-fA-F]+))");
    return re;
}

// ---------------------------------------------------------------------------------------------
// Module-level constant tables, mirroring CuInsParser.py's c_* module constants. (c_PosDepFuncs is
// intentionally not ported: it's defined in the original but never actually used there.)
// ---------------------------------------------------------------------------------------------

const std::map<char, std::string>& opPreModifierChar() {
    static const std::map<char, std::string> m{{'!', "cNOT"}, {'-', "cNEG"}, {'|', "cABS"}, {'~', "cINV"}};
    return m;
}

const std::set<std::string>& addrFuncs() {
    static const std::set<std::string> s{"BRA", "BRX", "BRXU", "CALL", "JMP", "JMX", "JMXU", "RET",
                                          "BSSY", "SSY", "CAL", "PRET", "PBK"};
    return s;
}

const std::set<std::string>& modiDTypes() {
    static const std::set<std::string> s{"S4", "S8", "S16", "S32", "S64", "U4", "U8", "U16", "U32", "U64", "F16", "F32", "F64"};
    return s;
}

const std::set<std::string>& modiDTypesExt() {
    static const std::set<std::string> s = [] {
        std::set<std::string> ext = modiDTypes();
        ext.insert({"S24", "U24", "S16H0", "S16H1", "U16H0", "U16H1"});
        return ext;
    }();
    return s;
}

const std::set<std::string>& modiRGBA() {
    static const std::set<std::string> s{"R", "G", "B", "A"};
    return s;
}

const std::set<std::string>& modiLOP() {
    static const std::set<std::string> s{"AND", "OR", "XOR", "NOT"};
    return s;
}

const std::map<std::string, std::set<std::string>>& posDepModis() {
    static const std::map<std::string, std::set<std::string>> m{
        {"I2I", modiDTypes()},     {"F2F", modiDTypes()},     {"I2IP", modiDTypes()},    {"F2FP", modiDTypes()},

        {"VADD", modiDTypes()},    {"VMAD", modiDTypes()},    {"VSHL", modiDTypes()},    {"VSHR", modiDTypes()},
        {"VSET", modiDTypes()},    {"VSETP", modiDTypes()},   {"VMNMX", modiDTypes()},   {"VABSDIFF", modiDTypes()},
        {"VABSDIFF4", modiDTypes()},

        {"XMAD", modiDTypesExt()}, {"IMAD", modiDTypesExt()}, {"IMAD32I", modiDTypesExt()}, {"IMADSP", modiDTypesExt()},
        {"IMUL", modiDTypesExt()}, {"IMUL32I", modiDTypesExt()},

        {"PSET", modiLOP()},       {"PSETP", modiLOP()},

        {"IDP", modiDTypes()},     {"HMMA", modiDTypes()},    {"IMMA", modiDTypes()},

        {"TLD4", modiRGBA()},
    };
    return m;
}

const std::set<std::string>& cvtOpcodes() {
    static const std::set<std::string> s{"I2F", "I2I", "F2I", "F2F", "I2FP", "I2IP", "F2IP", "F2FP", "FRND"};
    return s;
}

} // namespace

std::map<int, CuInsParser> CuInsParser::s_staticParsers;

CuInsParser::CuInsParser(const std::string& arch) : m_Arch(arch) {
    reset();
}

void CuInsParser::reset() {
    m_InsAddr = 0;
    m_InsString.clear();
    m_CTrString.clear();
    m_InsCode = 0;

    m_InsKey.clear();
    m_InsOp.clear();
    m_InsOpFull.clear();
    m_InsPredVal = 0;
    m_InsPredStr.clear();
    m_InsModifier.clear();
    m_InsVals.clear();
    m_InsTags.clear();

    m_RPList.clear();
}

CuInsParser& CuInsParser::getStaticParser(const std::string& arch) {
    const int vnum = CuSMVersion(arch).getVersionNumber();
    auto it = s_staticParsers.find(vnum);
    if (it == s_staticParsers.end()) {
        it = s_staticParsers.emplace(vnum, CuInsParser(arch)).first;
    }
    return it->second;
}

std::tuple<std::string, std::vector<std::int64_t>, std::vector<std::string>> CuInsParser::parse(const std::string& s,
                                                                                                   std::uint64_t addr,
                                                                                                   std::uint64_t code) {
    reset();
    m_InsString = strip(s);
    m_CTrString = constTr(m_InsString);

    std::smatch m;
    if (!std::regex_search(m_CTrString, m, insPatternRe())) {
        throw std::runtime_error("Unrecognized asm \"" + s + "\"");
    }

    m_InsAddr = addr;
    m_InsCode = code;
    m_InsPredStr = m[1].matched ? m.str(1) : std::string();

    m_InsOpFull = m.str(2);
    const std::vector<std::string> opTokens = splitChar(m_InsOpFull, '.');
    m_InsKey = opTokens[0];
    m_InsOp = opTokens[0];

    m_InsPredVal = parsePred(m_InsPredStr);
    m_InsVals = {m_InsPredVal};
    m_InsTags = {"P.Guard"};
    m_InsModifier.clear();
    for (const std::string& tok : opTokens) {
        m_InsModifier.push_back("0_" + tok);
    }

    const std::string operands = preprocessOperands(m.str(3));
    if (!operands.empty()) {
        const std::vector<std::string> operandList = splitChar(operands, ',');
        for (std::size_t i = 0; i < operandList.size(); ++i) {
            OperandParse op = parseOperand(operandList[i]);
            m_InsKey += "_" + op.type;
            m_InsVals.insert(m_InsVals.end(), op.val.begin(), op.val.end());
            m_InsTags.insert(m_InsTags.end(), op.tag.begin(), op.tag.end());
            for (const std::string& mo : op.modi) {
                m_InsModifier.push_back(std::to_string(i + 1) + "_" + mo);
            }
        }
    }

    specialTreatment();
    return {m_InsKey, m_InsVals, m_InsModifier};
}

std::string CuInsParser::constTr(const std::string& sIn) const {
    std::string s = stripComments(sIn);

    s = regexReplaceUnlessPrecededByDot(s, rzRe(), "R255");
    s = std::regex_replace(s, urzRe(), "UR63");
    s = std::regex_replace(s, ptRe(), "P7");
    s = std::regex_replace(s, uptRe(), "UP7");
    s = std::regex_replace(s, qnanRe(), "NAN");

    std::smatch m;
    if (std::regex_search(s, m, sbSetRe())) {
        const std::string sbValStr = transScoreboardSet(m.str(0));
        s = std::regex_replace(s, sbSetRe(), sbValStr);
    }

    s = std::regex_replace(s, whiteSpaceRe(), " ");
    s = removeInsignificantSpaces(s);

    return stripChars(s, " {};");
}

std::string CuInsParser::preprocessOperands(const std::string& sIn) const {
    std::string s = strip(sIn);
    if (addrFuncs().count(m_InsOp) > 0) {
        std::smatch m;
        if (std::regex_search(s, m, rImmeAddrRe())) {
            s = replaceAll(s, m.str(0), m.str(1) + "," + m.str(2));
        }
    }
    return s;
}

CuInsParser::OperandParse CuInsParser::parseOperand(const std::string& operand) {
    const ModifierSplit stripped = stripModifier(operand);
    const std::string& op = stripped.opMain;
    const std::vector<std::string>& modi = stripped.opModi;

    OperandParse result;

    if (std::regex_search(op, indexedPatternRe())) {
        const IndexedToken tok = parseIndexedToken(op);
        result.type = tok.label;
        result.val = tok.val;
        result.modi = modi;
        result.modi.insert(result.modi.end(), tok.modi.begin(), tok.modi.end());
        result.tag = {tok.label};
    } else if (!op.empty() && op.front() == '[') {
        result = parseAddress(op);
    } else if (op.starts_with("c[")) {
        result = parseConstMemory(op);
        result.modi.insert(result.modi.end(), modi.begin(), modi.end());
    } else if (op.starts_with("0x")) {
        result.type = "II";
        const ModifierSplit imme = stripImmeModifier(operand);
        const ImmeParse iv = parseIntImme(imme.opMain);
        result.val = iv.val;
        result.modi = iv.modi;
        result.modi.insert(result.modi.end(), imme.opModi.begin(), imme.opModi.end());
        result.tag = {result.type};
    } else if (std::regex_search(operand, fiTypeRe())) {
        result.type = "FI";
        const ModifierSplit imme = stripImmeModifier(operand);
        const ImmeParse iv = parseFloatImme(imme.opMain);
        result.val = iv.val;
        result.modi = iv.modi;
        result.modi.insert(result.modi.end(), imme.opModi.begin(), imme.opModi.end());
        result.tag = {result.type};
    } else if (op.starts_with("desc")) {
        result = parseDescAddress(op);
        result.modi.insert(result.modi.end(), modi.begin(), modi.end());
    } else if (op.starts_with("cx[")) {
        result = parseURConstMemory(op);
        result.modi.insert(result.modi.end(), modi.begin(), modi.end());
    } else {
        result.type = "L";
        result.val = {};
        result.modi = {operand};
        result.tag = {};
    }

    return result;
}

CuInsParser::IndexedToken CuInsParser::parseIndexedToken(const std::string& s) {
    const ModifierSplit ms = stripModifier(s);
    std::smatch m;
    if (!std::regex_search(ms.opMain, m, indexedPatternRe())) {
        throw std::runtime_error("Unknown indexedToken \"" + s + "\" in \"" + m_InsString + "\"");
    }

    const std::string t = m.str(1);
    const std::int64_t v = std::stoll(m.str(2), nullptr, 10);

    m_RPList.emplace_back(t, v);

    return {t, {v}, ms.opModi};
}

std::int64_t CuInsParser::parsePred(const std::string& s) {
    if (s.empty()) {
        if (!m_InsOp.empty() && m_InsOp.front() == 'U') {
            m_RPList.emplace_back("UP", 7);
        } else {
            m_RPList.emplace_back("P", 7);
        }
        return 7;
    }

    const IndexedToken tok = parseIndexedToken(stripChars(s, "@! "));
    if (s.find('!') != std::string::npos) {
        return tok.val[0] + 8;
    }
    return tok.val[0];
}

CuInsParser::ImmeParse CuInsParser::parseFloatImme(const std::string& s) const {
    char prec = 'F';
    const char p = m_InsOp.empty() ? '\0' : m_InsOp.front();
    if (p == 'H' || p == 'F' || p == 'D') {
        prec = p;
    } else if (m_InsOp == "MUFU" || m_InsOp == "RRO") {
        if (m_InsOpFull.find("64") != std::string::npos) {
            prec = 'D';
        } else if (m_InsOpFull.find("16") != std::string::npos) {
            prec = 'H';
        } else {
            prec = 'F';
        }
    } else {
        dumpInfo();
        throw std::runtime_error("Unknown float precision (" + m_InsOp + ")!");
    }

    const int nbits = m_InsOp.ends_with("32I") ? 32 : -1;

    const auto [v, modi] = m_Arch.convertFloatImme(s, prec, nbits);
    return {{static_cast<std::int64_t>(v)}, modi};
}

CuInsParser::ImmeParse CuInsParser::parseIntImme(const std::string& s) const {
    const std::int64_t i = parseHexSigned(s);
    if (i >= 0) {
        const auto [val, modi] = m_Arch.splitIntImmeModifier(m_InsOp, i);
        return {{val}, modi};
    }
    return {{i}, {"NegIntImme"}};
}

CuInsParser::OperandParse CuInsParser::parseConstMemory(const std::string& s) {
    const ModifierSplit ms = stripModifier(s);
    std::smatch m;
    if (!std::regex_search(ms.opMain, m, constMemTypeRe())) {
        throw std::runtime_error("Invalid constant memory operand: " + s);
    }

    OperandParse result;
    result.val = {parseHexSigned(m.str(1))};
    result.tag = {"Imme.CBank"};
    result.modi = ms.opModi;

    const OperandParse addr = parseAddress(m.str(2));
    result.type = "c" + addr.type;
    result.val.insert(result.val.end(), addr.val.begin(), addr.val.end());
    result.modi.insert(result.modi.end(), addr.modi.begin(), addr.modi.end());
    result.tag.insert(result.tag.end(), addr.tag.begin(), addr.tag.end());

    return result;
}

CuInsParser::OperandParse CuInsParser::parseURConstMemory(const std::string& s) {
    const ModifierSplit ms = stripModifier(s); // ms.opModi is intentionally discarded, mirroring the
                                                // original's shadowing of `opmodi` right after this call.
    std::smatch m;
    if (!std::regex_search(ms.opMain, m, urConstMemTypeRe())) {
        throw std::runtime_error("Invalid UR constant memory operand: " + s);
    }

    const IndexedToken bank = parseIndexedToken(m.str(1));

    OperandParse result;
    result.val = bank.val;
    for (const std::string& mo : bank.modi) {
        result.modi.push_back(bank.label + "_" + mo);
    }
    result.tag = {"UR.CBank"};

    const OperandParse addr = parseAddress(m.str(2));
    result.type = "cx" + addr.type;
    result.val.insert(result.val.end(), addr.val.begin(), addr.val.end());
    result.modi.insert(result.modi.end(), addr.modi.begin(), addr.modi.end());
    result.tag.insert(result.tag.end(), addr.tag.begin(), addr.tag.end());

    return result;
}

CuInsParser::OperandParse CuInsParser::parseDescAddress(const std::string& s) {
    std::smatch m;
    if (!std::regex_search(s, m, descAddressTypeRe())) {
        throw std::runtime_error("Invalid desc address operand: " + s);
    }

    const IndexedToken idx = parseIndexedToken(m.str(1)); // label/modi are intentionally discarded here too.

    OperandParse result;
    result.val = idx.val;
    result.tag = {"UR.Desc"};

    const OperandParse addr = parseAddress(m.str(2));
    result.type = "d" + addr.type;
    result.val.insert(result.val.end(), addr.val.begin(), addr.val.end());
    result.tag.insert(result.tag.end(), addr.tag.begin(), addr.tag.end());
    result.modi = addr.modi;

    return result;
}

std::string CuInsParser::transScoreboardSet(const std::string& s) const {
    const std::vector<std::string> parts = splitChar(stripChars(s, "{}"), ',');
    std::int64_t v = 0;
    for (const std::string& bs : parts) {
        v += (std::int64_t(1) << std::stoll(bs, nullptr, 10));
    }
    return "SBSET" + std::to_string(v);
}

CuInsParser::OperandParse CuInsParser::parseAddress(const std::string& s) {
    const std::string withPlus = insertPlusBeforeNegHex(s);
    const std::vector<std::string> tokens = splitChar(stripChars(withPlus, "[]"), '+');

    struct Entry {
        std::vector<std::int64_t> val;
        std::vector<std::string> modi;
    };
    std::map<std::string, Entry> pdict;

    for (const std::string& ts : tokens) {
        if (ts.find("0x") != std::string::npos) {
            const ImmeParse iv = parseIntImme(ts);
            pdict["I"] = Entry{iv.val, iv.modi};
        } else if (ts.empty()) {
            continue;
        } else {
            const IndexedToken tok = parseIndexedToken(ts);
            std::vector<std::string> prefixedModi;
            prefixedModi.reserve(tok.modi.size());
            for (const std::string& mo : tok.modi) {
                prefixedModi.push_back(tok.label + "." + mo);
            }
            pdict[tok.label] = Entry{tok.val, prefixedModi};
        }
    }

    OperandParse result;
    result.type = "A";

    if (const auto it = pdict.find("R"); it != pdict.end()) {
        result.type += "R";
        result.val.insert(result.val.end(), it->second.val.begin(), it->second.val.end());
        result.modi.insert(result.modi.end(), it->second.modi.begin(), it->second.modi.end());
        result.tag.push_back("R.Addr");
    }

    if (const auto it = pdict.find("UR"); it != pdict.end()) {
        result.type += "UR";
        result.val.insert(result.val.end(), it->second.val.begin(), it->second.val.end());
        result.modi.insert(result.modi.end(), it->second.modi.begin(), it->second.modi.end());
        result.tag.push_back("UR.Addr");
    }

    if (const auto it = pdict.find("I"); it != pdict.end()) {
        result.type += "I";
        result.val.insert(result.val.end(), it->second.val.begin(), it->second.val.end());
        result.modi.insert(result.modi.end(), it->second.modi.begin(), it->second.modi.end());
        result.tag.push_back("Imme.Addr");
    } else {
        result.type += "I";
        result.val.push_back(0);
        result.tag.push_back("Imme.Addr");
    }

    return result;
}

void CuInsParser::specialTreatment() {
    if (m_InsOp == "PLOP3" || m_InsOp == "UPLOP3") {
        const std::int64_t v = m_InsVals[m_InsVals.size() - 2];
        m_InsVals[m_InsVals.size() - 2] = (v & 7) + ((v & 0xf8) << 5);
    } else if (cvtOpcodes().count(m_InsOp) > 0) {
        if (m_InsOpFull.find("64") != std::string::npos) {
            m_InsModifier.push_back("0_CVT64");
        }
    } else if (addrFuncs().count(m_InsOp) > 0) {
        if (m_InsKey.ends_with("_II")) {
            if (m_InsOpFull.find("ABS") == std::string::npos) {
                const std::int64_t addr = m_InsVals.back() - static_cast<std::int64_t>(m_InsAddr) - m_Arch.getInstructionLength();
                if (addr < 0) {
                    m_InsModifier.push_back("0_NegAddrOffset");
                }
                m_InsVals.back() = addr;
            }
        }
    }

    if (m_Arch.getPosDepOpcodes().count(m_InsOp) > 0) {
        const auto& modiSet = posDepModis().at(m_InsOp);
        int counter = 0;
        for (std::string& m : m_InsModifier) {
            if (m.starts_with("0_") && modiSet.count(m.substr(2)) > 0) {
                m += "@" + std::to_string(counter);
                ++counter;
            }
        }
    }
}

CuInsParser::ModifierSplit CuInsParser::stripModifier(const std::string& s) const {
    std::smatch m;
    if (!std::regex_search(s, m, modifierPatternRe())) {
        throw std::runtime_error("Unknown token " + s);
    }

    const std::string pre = m.str(1);
    const std::string post = m.str(3);
    const std::string opMain = m.str(2);

    std::vector<std::string> opModi;
    for (const char c : pre) {
        opModi.push_back(opPreModifierChar().at(c));
    }
    for (const std::string& c : splitChar(post, '.')) {
        if (c.empty()) {
            continue;
        }
        opModi.push_back(c);
    }

    return {opMain, opModi};
}

CuInsParser::ModifierSplit CuInsParser::stripImmeModifier(const std::string& s) const {
    std::smatch m;
    if (!std::regex_search(s, m, immeModiRe())) {
        throw std::runtime_error("Unknown immediate string: " + s);
    }

    const std::string sval = m.str(1);
    const std::string modisRaw = m.str(2);

    std::vector<std::string> modis;
    if (!modisRaw.empty()) {
        modis = splitChar(lstripChars(modisRaw, "."), '.');
    }

    return {sval, modis};
}

void CuInsParser::dumpInfo() const {
    std::ostringstream idOss;
    idOss << "0x" << std::hex << std::setfill('0') << std::setw(16) << reinterpret_cast<std::uintptr_t>(this);

    std::cout << "#### CuInsParser @ " << idOss.str() << " ####" << std::endl;
    std::cout << "  InsString : " << m_InsString << std::endl;
    std::cout << "  CTrString : " << m_CTrString << std::endl;
    std::cout << "  InsAddr   : 0x" << std::hex << m_InsAddr << std::dec << std::endl;
    std::cout << "  InsPred   : " << m_InsPredStr << " (0x" << std::hex << m_InsPredVal << std::dec << ")" << std::endl;
    std::cout << "  InsCode   : " << m_Arch.formatCode(BigInt(m_InsCode)) << std::endl;
    std::cout << "  InsOp     : " << m_InsOp << std::endl;
    std::cout << "  InsOpFull : " << m_InsOpFull << std::endl;
    std::cout << "  InsKey    : " << m_InsKey << std::endl;
    std::cout << "  InsVals   : " << intList2Str(m_InsVals) << std::endl;
    std::cout << "  InsTags   : " << pyStrListRepr(m_InsTags) << std::endl;

    const std::size_t n = std::min(m_InsTags.size(), m_InsVals.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::string& t = m_InsTags[i];
        std::int64_t v = m_InsVals[i];
        const std::string t0 = firstDotSegment(t);
        if (t0 == "R" || t0 == "P" || t0 == "UP" || t0 == "UR") {
            if (t == "P.Guard") {
                v &= 0x7;
            }
            std::cout << "      " << std::left << std::setw(10) << t << std::right << " = " << t0 << v << std::endl;
        } else {
            std::cout << "      " << std::left << std::setw(10) << t << std::right << " = 0x" << std::hex << v << std::dec
                       << std::endl;
        }
    }

    std::cout << "  InsModi   : " << pyStrListRepr(m_InsModifier) << std::endl;

    std::string rps;
    for (std::size_t i = 0; i < m_RPList.size(); ++i) {
        rps += m_RPList[i].first + std::to_string(m_RPList[i].second);
        if (i + 1 < m_RPList.size()) {
            rps += ", ";
        }
    }
    std::cout << "  RPList    : [" << rps << "]" << std::endl;
    std::cout << std::endl;
}

CuInsParser::DumpInfoDict CuInsParser::dumpInfoAsDict() const {
    DumpInfoDict d;
    d.insString = m_InsString;
    d.cTrString = m_CTrString;
    d.insAddr = m_InsAddr;
    d.insPredStr = m_InsPredStr;
    d.insPredVal = m_InsPredVal;
    d.insCode = m_InsCode;
    d.insOp = m_InsOp;
    d.insOpFull = m_InsOpFull;
    d.insKey = m_InsKey;
    d.insVals = m_InsVals;
    d.insTags = m_InsTags;
    d.insModi = m_InsModifier;
    d.rpList = m_RPList;
    return d;
}

} // namespace CuAsm
