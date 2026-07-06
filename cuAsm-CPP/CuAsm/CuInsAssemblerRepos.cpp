#include "CuInsAssemblerRepos.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "CuAsmLogger.hpp"
#include "config.hpp"

namespace CuAsm {

namespace {

/** @brief python repr() of a plain string, mirroring the single-quoted string literals used
 *         throughout CuInsAssemblerRepos.__repr__. */
std::string pyStrRepr(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += "'";
    return out;
}

/** @brief python '%#08x'-style formatting of a std::uint64_t. */
std::string hexFixedWidth(std::uint64_t v, int width) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setfill('0') << std::setw(std::max(0, width - 2)) << v;
    return oss.str();
}

// ---------------------------------------------------------------------------------------------
// A minimal recursive-descent parser for the literal syntax CuInsAssembler::repr()/
// CuInsAssemblerRepos::repr() produce (the same syntax python's initFromFile() feeds to eval()),
// used to reload a previously saved repos file. Only the subset of python literal syntax actually
// emitted by those repr() methods is supported: None, (possibly hex/negative) integers, "a/b"
// rational literals, single/double-quoted strings, lists, dicts, tuples (parsed as lists), and the
// "Name(args)"/"Name(kwarg=value)" call forms used for CuInsAssemblerRepos(...), CuInsAssembler(...),
// CuSMVersion(...) and Matrix(...).
// ---------------------------------------------------------------------------------------------

struct Token {
    enum class Type { Ident, Str, Num, LParen, RParen, LBrack, RBrack, LBrace, RBrace, Colon, Comma, Equals, Slash, End } type;
    std::string text;
};

class Tokenizer {
public:
    explicit Tokenizer(const std::string& src) : m_Src(src) { advance(); }

    const Token& peek() const { return m_Current; }

    Token take() {
        Token t = m_Current;
        advance();
        return t;
    }

    Token takeExpected(Token::Type type, const char* what) {
        if (m_Current.type != type) {
            throw std::runtime_error(std::string("InsAsmRepos parse error: expected ") + what + " but got \"" + m_Current.text + "\"");
        }
        return take();
    }

private:
    void advance() {
        skipWhitespaceAndComments();
        if (m_Pos >= m_Src.size()) {
            m_Current = {Token::Type::End, ""};
            return;
        }

        const char c = m_Src[m_Pos];
        switch (c) {
            case '(': m_Current = {Token::Type::LParen, "("}; ++m_Pos; return;
            case ')': m_Current = {Token::Type::RParen, ")"}; ++m_Pos; return;
            case '[': m_Current = {Token::Type::LBrack, "["}; ++m_Pos; return;
            case ']': m_Current = {Token::Type::RBrack, "]"}; ++m_Pos; return;
            case '{': m_Current = {Token::Type::LBrace, "{"}; ++m_Pos; return;
            case '}': m_Current = {Token::Type::RBrace, "}"}; ++m_Pos; return;
            case ':': m_Current = {Token::Type::Colon, ":"}; ++m_Pos; return;
            case ',': m_Current = {Token::Type::Comma, ","}; ++m_Pos; return;
            case '=': m_Current = {Token::Type::Equals, "="}; ++m_Pos; return;
            case '/': m_Current = {Token::Type::Slash, "/"}; ++m_Pos; return;
            case '\'':
            case '"': m_Current = readString(c); return;
            default: break;
        }

        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            m_Current = readNumber();
            return;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            m_Current = readIdent();
            return;
        }

        throw std::runtime_error(std::string("InsAsmRepos parse error: unexpected character '") + c + "'");
    }

    void skipWhitespaceAndComments() {
        for (;;) {
            while (m_Pos < m_Src.size() && std::isspace(static_cast<unsigned char>(m_Src[m_Pos]))) {
                ++m_Pos;
            }
            if (m_Pos < m_Src.size() && m_Src[m_Pos] == '#') {
                while (m_Pos < m_Src.size() && m_Src[m_Pos] != '\n') {
                    ++m_Pos;
                }
                continue;
            }
            break;
        }
    }

    Token readString(char quote) {
        std::string out;
        ++m_Pos;
        while (m_Pos < m_Src.size() && m_Src[m_Pos] != quote) {
            if (m_Src[m_Pos] == '\\' && m_Pos + 1 < m_Src.size()) {
                ++m_Pos;
                out += m_Src[m_Pos];
            } else {
                out += m_Src[m_Pos];
            }
            ++m_Pos;
        }
        if (m_Pos >= m_Src.size()) {
            throw std::runtime_error("InsAsmRepos parse error: unterminated string literal");
        }
        ++m_Pos;
        return {Token::Type::Str, out};
    }

    Token readNumber() {
        const std::size_t start = m_Pos;
        if (m_Src[m_Pos] == '-') {
            ++m_Pos;
        }
        if (m_Pos + 1 < m_Src.size() && m_Src[m_Pos] == '0' && (m_Src[m_Pos + 1] == 'x' || m_Src[m_Pos + 1] == 'X')) {
            m_Pos += 2;
            while (m_Pos < m_Src.size() && std::isxdigit(static_cast<unsigned char>(m_Src[m_Pos]))) {
                ++m_Pos;
            }
        } else {
            while (m_Pos < m_Src.size() && std::isdigit(static_cast<unsigned char>(m_Src[m_Pos]))) {
                ++m_Pos;
            }
        }
        return {Token::Type::Num, m_Src.substr(start, m_Pos - start)};
    }

    Token readIdent() {
        const std::size_t start = m_Pos;
        while (m_Pos < m_Src.size() && (std::isalnum(static_cast<unsigned char>(m_Src[m_Pos])) || m_Src[m_Pos] == '_')) {
            ++m_Pos;
        }
        return {Token::Type::Ident, m_Src.substr(start, m_Pos - start)};
    }

    std::string m_Src;
    std::size_t m_Pos = 0;
    Token m_Current{Token::Type::End, ""};
};

BigInt parseBigIntToken(const std::string& tok) {
    const bool neg = !tok.empty() && tok[0] == '-';
    const BigInt magnitude(tok.substr(neg ? 1 : 0));
    return neg ? -magnitude : magnitude;
}

BigRational parseNumberOrFraction(Tokenizer& tz) {
    const BigInt num = parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text);
    if (tz.peek().type == Token::Type::Slash) {
        tz.take();
        const BigInt den = parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text);
        return BigRational(num, den);
    }
    return BigRational(num);
}

std::vector<std::string> parseStrList(Tokenizer& tz) {
    std::vector<std::string> out;
    tz.takeExpected(Token::Type::LBrack, "[");
    while (tz.peek().type != Token::Type::RBrack) {
        out.push_back(tz.takeExpected(Token::Type::Str, "string").text);
        if (tz.peek().type == Token::Type::Comma) {
            tz.take();
        }
    }
    tz.take();
    return out;
}

InsVals parseIntList(Tokenizer& tz) {
    InsVals out;
    tz.takeExpected(Token::Type::LBrack, "[");
    while (tz.peek().type != Token::Type::RBrack) {
        out.push_back(parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text));
        if (tz.peek().type == Token::Type::Comma) {
            tz.take();
        }
    }
    tz.take();
    return out;
}

std::vector<InsReposEntry> parseInsReposList(Tokenizer& tz) {
    std::vector<InsReposEntry> out;
    tz.takeExpected(Token::Type::LBrack, "[");
    while (tz.peek().type != Token::Type::RBrack) {
        tz.takeExpected(Token::Type::LParen, "(");
        InsVals vals = parseIntList(tz);
        tz.takeExpected(Token::Type::Comma, ",");
        InsModi modi = parseStrList(tz);
        tz.takeExpected(Token::Type::Comma, ",");
        const BigInt code = parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text);
        tz.takeExpected(Token::Type::RParen, ")");
        out.push_back(InsReposEntry{std::move(vals), std::move(modi), code});
        if (tz.peek().type == Token::Type::Comma) {
            tz.take();
        }
    }
    tz.take();
    return out;
}

ModiSet parseModiSetDict(Tokenizer& tz) {
    std::vector<std::pair<std::string, int>> entries;
    tz.takeExpected(Token::Type::LBrace, "{");
    while (tz.peek().type != Token::Type::RBrace) {
        const std::string name = tz.takeExpected(Token::Type::Str, "string").text;
        tz.takeExpected(Token::Type::Colon, ":");
        const int idx = static_cast<int>(parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text).convert_to<long long>());
        entries.emplace_back(name, idx);
        if (tz.peek().type == Token::Type::Comma) {
            tz.take();
        }
    }
    tz.take();

    ModiSet result;
    for (const auto& [name, idx] : entries) {
        if (static_cast<int>(result.size()) <= idx) {
            result.resize(static_cast<std::size_t>(idx) + 1);
        }
        result[static_cast<std::size_t>(idx)] = name;
    }
    return result;
}

RationalMatrix parseMatrix(Tokenizer& tz) {
    const Token ident = tz.takeExpected(Token::Type::Ident, "Matrix");
    if (ident.text != "Matrix") {
        throw std::runtime_error("InsAsmRepos parse error: expected Matrix(...), got " + ident.text + "(...)");
    }
    tz.takeExpected(Token::Type::LParen, "(");

    if (tz.peek().type == Token::Type::Num) {
        // Empty-matrix special case: "Matrix(0, 0, [])".
        tz.take();
        tz.takeExpected(Token::Type::Comma, ",");
        tz.take();
        tz.takeExpected(Token::Type::Comma, ",");
        tz.takeExpected(Token::Type::LBrack, "[");
        tz.takeExpected(Token::Type::RBrack, "]");
        tz.takeExpected(Token::Type::RParen, ")");
        return RationalMatrix();
    }

    tz.takeExpected(Token::Type::LBrack, "[");
    std::vector<std::vector<BigRational>> rows;
    while (tz.peek().type != Token::Type::RBrack) {
        tz.takeExpected(Token::Type::LBrack, "[");
        std::vector<BigRational> row;
        while (tz.peek().type != Token::Type::RBrack) {
            row.push_back(parseNumberOrFraction(tz));
            if (tz.peek().type == Token::Type::Comma) {
                tz.take();
            }
        }
        tz.take();
        rows.push_back(std::move(row));
        if (tz.peek().type == Token::Type::Comma) {
            tz.take();
        }
    }
    tz.take();
    tz.takeExpected(Token::Type::RParen, ")");
    return RationalMatrix::fromRows(rows);
}

CuSMVersion parseCuSMVersion(Tokenizer& tz) {
    const Token ident = tz.takeExpected(Token::Type::Ident, "CuSMVersion");
    if (ident.text != "CuSMVersion") {
        throw std::runtime_error("InsAsmRepos parse error: expected CuSMVersion(...), got " + ident.text + "(...)");
    }
    tz.takeExpected(Token::Type::LParen, "(");
    const int version = static_cast<int>(parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text).convert_to<long long>());
    tz.takeExpected(Token::Type::RParen, ")");
    return CuSMVersion(version);
}

std::vector<InsInfo> parseInsInfoList(Tokenizer& tz) {
    std::vector<InsInfo> out;
    tz.takeExpected(Token::Type::LBrack, "[");
    while (tz.peek().type != Token::Type::RBrack) {
        tz.takeExpected(Token::Type::LParen, "(");
        const BigInt addr = parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text);
        tz.takeExpected(Token::Type::Comma, ",");
        const BigInt code = parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text);
        tz.takeExpected(Token::Type::Comma, ",");
        const std::string asmText = tz.takeExpected(Token::Type::Str, "string").text;
        tz.takeExpected(Token::Type::RParen, ")");
        out.push_back(InsInfo{addr.convert_to<std::uint64_t>(), code, asmText});
        if (tz.peek().type == Token::Type::Comma) {
            tz.take();
        }
    }
    tz.take();
    return out;
}

std::map<BigInt, InsInfo> parseErrRecordsDict(Tokenizer& tz) {
    std::map<BigInt, InsInfo> out;
    tz.takeExpected(Token::Type::LBrace, "{");
    while (tz.peek().type != Token::Type::RBrace) {
        const BigInt key = parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text);
        tz.takeExpected(Token::Type::Colon, ":");
        tz.takeExpected(Token::Type::LParen, "(");
        const BigInt addr = parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text);
        tz.takeExpected(Token::Type::Comma, ",");
        const BigInt code = parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text);
        tz.takeExpected(Token::Type::Comma, ",");
        const std::string asmText = tz.takeExpected(Token::Type::Str, "string").text;
        tz.takeExpected(Token::Type::RParen, ")");
        out[key] = InsInfo{addr.convert_to<std::uint64_t>(), code, asmText};
        if (tz.peek().type == Token::Type::Comma) {
            tz.take();
        }
    }
    tz.take();
    return out;
}

CuInsAssemblerState parseCuInsAssembler(Tokenizer& tz) {
    const Token ident = tz.takeExpected(Token::Type::Ident, "CuInsAssembler");
    if (ident.text != "CuInsAssembler") {
        throw std::runtime_error("InsAsmRepos parse error: expected CuInsAssembler(...), got " + ident.text + "(...)");
    }
    tz.takeExpected(Token::Type::LParen, "(");
    tz.takeExpected(Token::Type::Str, "string"); // positional InsKey placeholder arg, unused
    tz.takeExpected(Token::Type::Comma, ",");
    tz.takeExpected(Token::Type::LBrace, "{");

    std::string insKey;
    std::vector<InsReposEntry> insRepos;
    ModiSet insModiSet;
    RationalMatrix valMatrix;
    RationalMatrix pSol;
    BigInt pSolFac(1);
    RationalMatrix valNullMat;
    RationalMatrix rhs;
    std::vector<InsInfo> insRecords;
    std::map<BigInt, InsInfo> errRecords;
    std::optional<CuSMVersion> arch;

    while (tz.peek().type != Token::Type::RBrace) {
        const std::string field = tz.takeExpected(Token::Type::Str, "string").text;
        tz.takeExpected(Token::Type::Colon, ":");

        if (field == "InsKey") {
            insKey = tz.takeExpected(Token::Type::Str, "string").text;
        } else if (field == "InsRepos") {
            insRepos = parseInsReposList(tz);
        } else if (field == "InsModiSet") {
            insModiSet = parseModiSetDict(tz);
        } else if (field == "ValMatrix") {
            valMatrix = parseMatrix(tz);
        } else if (field == "PSol") {
            pSol = parseMatrix(tz);
        } else if (field == "PSolFac") {
            pSolFac = parseBigIntToken(tz.takeExpected(Token::Type::Num, "number").text);
        } else if (field == "ValNullMat") {
            if (tz.peek().type == Token::Type::Ident && tz.peek().text == "None") {
                tz.take();
                valNullMat = RationalMatrix();
            } else {
                valNullMat = parseMatrix(tz);
            }
        } else if (field == "InsRecords") {
            insRecords = parseInsInfoList(tz);
        } else if (field == "ErrRecords") {
            errRecords = parseErrRecordsDict(tz);
        } else if (field == "Rhs") {
            rhs = parseMatrix(tz);
        } else if (field == "Arch") {
            arch = parseCuSMVersion(tz);
        } else {
            throw std::runtime_error("InsAsmRepos parse error: unknown CuInsAssembler field \"" + field + "\"");
        }

        if (tz.peek().type == Token::Type::Comma) {
            tz.take();
        }
    }
    tz.take(); // }
    tz.takeExpected(Token::Type::RParen, ")");

    if (!arch.has_value()) {
        throw std::runtime_error("InsAsmRepos parse error: CuInsAssembler is missing its \"Arch\" field");
    }

    return CuInsAssemblerState{insKey,
                                std::move(insRepos),
                                std::move(insModiSet),
                                std::move(valMatrix),
                                std::move(pSol),
                                pSolFac,
                                std::move(valNullMat),
                                std::move(rhs),
                                std::move(insRecords),
                                std::move(errRecords),
                                *arch};
}

CuInsAssemblerRepos::InsAsmDict parseReposFile(const std::string& contents) {
    Tokenizer tz(contents);

    const Token ident = tz.takeExpected(Token::Type::Ident, "CuInsAssemblerRepos");
    if (ident.text != "CuInsAssemblerRepos") {
        throw std::runtime_error("InsAsmRepos parse error: expected CuInsAssemblerRepos(...) at top level");
    }
    tz.takeExpected(Token::Type::LParen, "(");

    std::map<std::string, CuInsAssemblerState> states;
    tz.takeExpected(Token::Type::LBrace, "{");
    while (tz.peek().type != Token::Type::RBrace) {
        const std::string key = tz.takeExpected(Token::Type::Str, "string").text;
        tz.takeExpected(Token::Type::Colon, ":");
        states.emplace(key, parseCuInsAssembler(tz));
        if (tz.peek().type == Token::Type::Comma) {
            tz.take();
        }
    }
    tz.take(); // }

    tz.takeExpected(Token::Type::Comma, ",");
    const Token archKw = tz.takeExpected(Token::Type::Ident, "arch");
    if (archKw.text != "arch") {
        throw std::runtime_error("InsAsmRepos parse error: expected \"arch=\" keyword argument");
    }
    tz.takeExpected(Token::Type::Equals, "=");
    parseCuSMVersion(tz); // top-level repos arch; each assembler's own "Arch" field is authoritative
    tz.takeExpected(Token::Type::RParen, ")");

    CuInsAssemblerRepos::InsAsmDict dict;
    for (auto& [key, state] : states) {
        dict.emplace(key, CuInsAssembler(state));
    }
    return dict;
}

/** @brief Levenshtein-distance-based similarity ratio in [0, 1], approximating python's
 *         difflib.SequenceMatcher.ratio() closely enough for "did you mean" style suggestions. */
double similarityRatio(const std::string& a, const std::string& b) {
    const std::size_t la = a.size();
    const std::size_t lb = b.size();
    if (la == 0 && lb == 0) {
        return 1.0;
    }

    std::vector<std::vector<int>> dp(la + 1, std::vector<int>(lb + 1));
    for (std::size_t i = 0; i <= la; ++i) {
        dp[i][0] = static_cast<int>(i);
    }
    for (std::size_t j = 0; j <= lb; ++j) {
        dp[0][j] = static_cast<int>(j);
    }
    for (std::size_t i = 1; i <= la; ++i) {
        for (std::size_t j = 1; j <= lb; ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({dp[i - 1][j] + 1, dp[i][j - 1] + 1, dp[i - 1][j - 1] + cost});
        }
    }

    const int dist = dp[la][lb];
    return 1.0 - static_cast<double>(dist) / static_cast<double>(std::max(la, lb));
}

} // namespace

std::map<int, CuInsAssemblerRepos> CuInsAssemblerRepos::s_staticRepos;

CuInsAssemblerRepos::CuInsAssemblerRepos(std::optional<std::string> reposFile, std::optional<std::string> arch) {
    resetArch(arch.has_value() ? std::optional<CuSMVersion>(CuSMVersion(*arch)) : std::nullopt);
    if (reposFile.has_value()) {
        initFromFile(*reposFile);
    } else {
        reset();
    }
}

CuInsAssemblerRepos::CuInsAssemblerRepos(const CuSMVersion& arch) {
    resetArch(arch);
    reset();
}

void CuInsAssemblerRepos::resetArch(std::optional<CuSMVersion> arch) {
    if (arch.has_value()) {
        m_Arch = arch;
        m_InsParser.emplace("sm_" + std::to_string(arch->getVersionNumber()));
    } else {
        m_Arch.reset();
        m_InsParser.reset();
    }
}

void CuInsAssemblerRepos::convertArch(const CuSMVersion& arch) {
    if (m_Arch.has_value() && arch == *m_Arch) {
        return;
    }
    resetArch(arch);
    for (auto& [key, insAsm] : m_InsAsmDict) {
        (void)key;
        insAsm.m_Arch = arch;
    }
}

CuSMVersion CuInsAssemblerRepos::getSMVersion() const {
    return m_Arch.value();
}

std::string CuInsAssemblerRepos::getArchString() const {
    std::string s = m_Arch.value().getVersionString();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void CuInsAssemblerRepos::setToDefaultInsAsmDict() {
    const int vnum = m_Arch.value().getVersionNumber();
    const std::string fname = Config::getDefaultInsAsmReposFile(vnum);
    if (std::filesystem::exists(fname)) {
        initFromFile(fname);
        return;
    }

    const auto aliasIt = CuSMVersion::InsAsmReposAliasDict.find(vnum);
    if (aliasIt != CuSMVersion::InsAsmReposAliasDict.end()) {
        const int anum = aliasIt->second;
        const std::string aname = Config::getDefaultInsAsmReposFile(anum);
        if (std::filesystem::exists(aname)) {
            CuAsmLogger::logWarning("No default InsAsmRepos for SM_" + std::to_string(vnum) + " found! Use SM_" + std::to_string(anum) +
                                     " instead...");
            initFromFile(aname);
            convertArch(CuSMVersion(anum));
            return;
        }
    }

    CuAsmLogger::logError("No default or alias InsAsmRepos for SM_" + std::to_string(vnum) + " found! Use empty repos ...");
    reset();
}

CuInsAssemblerRepos CuInsAssemblerRepos::getDefaultRepos(const std::string& arch) {
    const CuSMVersion smArch(arch);
    CuInsAssemblerRepos repos(smArch);
    repos.setToDefaultInsAsmDict();
    return repos;
}

CuInsAssemblerRepos& CuInsAssemblerRepos::getStaticRepos(const std::string& arch) {
    const int vnum = CuSMVersion(arch).getVersionNumber();
    auto it = s_staticRepos.find(vnum);
    if (it == s_staticRepos.end()) {
        it = s_staticRepos.emplace(vnum, getDefaultRepos(arch)).first;
    }
    return it->second;
}

void CuInsAssemblerRepos::reset(std::optional<InsAsmDict> insAsmDict) {
    m_InsAsmDict = insAsmDict.has_value() ? std::move(*insAsmDict) : InsAsmDict();
}

CuInsAssembler& CuInsAssemblerRepos::operator[](const std::string& key) {
    return m_InsAsmDict.at(key);
}

const CuInsAssembler& CuInsAssemblerRepos::operator[](const std::string& key) const {
    return m_InsAsmDict.at(key);
}

bool CuInsAssemblerRepos::contains(const std::string& key) const {
    return m_InsAsmDict.find(key) != m_InsAsmDict.end();
}

std::size_t CuInsAssemblerRepos::size() const {
    return m_InsAsmDict.size();
}

void CuInsAssemblerRepos::initFromFile(const std::string& fileName) {
    std::ifstream fin(fileName);
    if (!fin) {
        throw std::runtime_error("Cannot open InsAsmRepos file " + fileName + "!");
    }
    std::ostringstream buf;
    buf << fin.rdbuf();

    m_InsAsmDict = parseReposFile(buf.str());

    for (const auto& [key, insAsm] : m_InsAsmDict) {
        if (!m_Arch.has_value()) {
            resetArch(insAsm.m_Arch);
        } else if (insAsm.m_Arch != *m_Arch) {
            CuAsmLogger::logWarning("InsAsm arch " + insAsm.m_Arch.getVersionString() + " of " + key + " does not match with repos " +
                                     m_Arch->getVersionString() + "!!! Resetting...");
            resetArch(insAsm.m_Arch);
        }
        break; // only the first insasm is checked, mirroring initFromFile's unconditional break
    }
}

BigInt CuInsAssemblerRepos::assemble(std::uint64_t addr, const std::string& s, bool precheck, bool showCandidates) {
    const auto [insKey, insValsU64, insModi] = m_InsParser->parse(s, addr, 0);
    const InsVals insVals(insValsU64.begin(), insValsU64.end());

    const auto it = m_InsAsmDict.find(insKey);
    if (it == m_InsAsmDict.end()) {
        std::string msg = "Unknown InsKey(" + insKey + ") in Repos!";
        if (showCandidates) {
            msg += "\n    Available InsKeys: \n" + getInsKeyCandidates(insKey);
        }
        throw std::runtime_error(msg);
    }

    CuInsAssembler& insAsm = it->second;
    if (precheck) {
        const CuInsAssembler::AssembleCheck check = insAsm.canAssemble(insVals, insModi);
        if (check.brief.has_value()) {
            std::string msg = "Assembling failed (" + *check.brief + "): " + check.info;
            if (showCandidates) {
                msg += "\n    Known Records:\n";
                for (const InsInfo& r : insAsm.iterRecords()) {
                    msg += "        " + r.asmText + "\n";
                }
            }
            throw std::runtime_error(msg);
        }
    }

    return insAsm.buildCode(insVals, insModi);
}

void CuInsAssemblerRepos::verify(CuInsFeeder& feeder) {
    CuAsmLogger::logTimeIt("CuInsAssemblerRepos::verify", [&]() {
        bool res = true;
        int cnt = 0;
        const auto t0 = std::chrono::steady_clock::now();

        while (const auto rec = feeder.next()) {
            ++cnt;
            try {
                const BigInt casm = assemble(rec->addr, rec->asmText);
                if (BigInt(rec->code) != casm) {
                    CuAsmLogger::logError("Error when verifying :");
                    CuAsmLogger::logError("  " + rec->asmText);
                    CuAsmLogger::logError("  CodeOrg: " + m_Arch->formatCode(BigInt(rec->code)));
                    CuAsmLogger::logError("  CodeAsm: " + m_Arch->formatCode(casm));
                }
            } catch (const std::exception& e) {
                CuAsmLogger::logError(e.what());
                CuAsmLogger::logError("Error when assembling :");
                CuAsmLogger::logError("  " + rec->asmText);
                res = false;
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();

        std::ostringstream msg;
        if (res) {
            msg << "Verified " << cnt << " ins in " << std::fixed << std::setprecision(3) << std::setw(8) << secs << " secs.";
            if (secs != 0.0) {
                msg << "  ~" << std::fixed << std::setprecision(2) << std::setw(8) << (static_cast<double>(cnt) / secs) << " ins/s.";
            }
            CuAsmLogger::logProcedure(msg.str());
        } else {
            msg << "Verifying failed in " << std::fixed << std::setprecision(3) << std::setw(8) << secs << " secs!!!";
            CuAsmLogger::logError(msg.str());
        }
    })();
}

int CuInsAssemblerRepos::updateFromGenerator(const std::function<std::optional<InsFeederRecord>()>& next, InsAsmDict& dict) {
    return CuAsmLogger::logTimeIt("CuInsAssemblerRepos::update", [&]() {
        const auto t0 = std::chrono::steady_clock::now();
        int cnt = 0;
        int ncnt = 0;

        while (const auto rec = next()) {
            ++cnt;
            const auto [insKey, insValsU64, insModi] = m_InsParser->parse(rec->asmText, rec->addr, rec->code.convert_to<std::uint64_t>());
            const InsVals insVals(insValsU64.begin(), insValsU64.end());

            auto it = dict.find(insKey);
            if (it == dict.end()) {
                it = dict.emplace(insKey, CuInsAssembler(insKey, "sm_" + std::to_string(m_Arch->getVersionNumber()))).first;
            }

            const InsInfo insInfo{rec->addr, rec->code, rec->asmText};
            const CuInsAssembler::PushResult res = it->second.push(insVals, insModi, rec->code, insInfo);

            if (res.info == "NewModi" || res.info == "NewVals" || res.info == "NewConflict") {
                ++ncnt;
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();

        std::ostringstream msg;
        msg << "Updated " << cnt << " ins (" << ncnt << " new) in " << std::fixed << std::setprecision(3) << std::setw(8) << secs
            << " secs .";
        if (secs != 0.0) {
            msg << "  ~" << std::fixed << std::setprecision(2) << std::setw(8) << (static_cast<double>(cnt) / secs) << " ins/s.";
        }
        CuAsmLogger::logProcedure(msg.str());

        return ncnt;
    })();
}

int CuInsAssemblerRepos::update(CuInsFeeder& feeder) {
    return updateFromGenerator(
        [&feeder]() -> std::optional<InsFeederRecord> {
            if (const auto rec = feeder.next()) {
                return InsFeederRecord{rec->addr, BigInt(rec->code), rec->asmText, 0};
            }
            return std::nullopt;
        },
        m_InsAsmDict);
}

void CuInsAssemblerRepos::rebuild() {
    CuAsmLogger::logTimeIt("CuInsAssemblerRepos::rebuild", [&]() {
        InsAsmDict tmp;
        const std::vector<InsFeederRecord> records = recordsFeeder();
        std::size_t idx = 0;
        updateFromGenerator(
            [&]() -> std::optional<InsFeederRecord> {
                if (idx < records.size()) {
                    return records[idx++];
                }
                return std::nullopt;
            },
            tmp);
        m_InsAsmDict = std::move(tmp);
    })();
}

void CuInsAssemblerRepos::mergeFrom(CuInsAssemblerRepos& repos) {
    const std::vector<InsFeederRecord> records = repos.recordsFeeder();
    std::size_t idx = 0;
    updateFromGenerator(
        [&]() -> std::optional<InsFeederRecord> {
            if (idx < records.size()) {
                return records[idx++];
            }
            return std::nullopt;
        },
        m_InsAsmDict);
}

void CuInsAssemblerRepos::merge(const std::string& reposFile) {
    CuAsmLogger::logTimeIt("CuInsAssemblerRepos::merge", [&]() {
        CuInsAssemblerRepos repos(reposFile);
        mergeFrom(repos);
    })();
}

void CuInsAssemblerRepos::merge(CuInsAssemblerRepos& reposSource) {
    CuAsmLogger::logTimeIt("CuInsAssemblerRepos::merge", [&]() { mergeFrom(reposSource); })();
}

void CuInsAssemblerRepos::completePredCodes() {
    const std::vector<InsFeederRecord> records = genPredRecords();
    std::size_t idx = 0;
    updateFromGenerator(
        [&]() -> std::optional<InsFeederRecord> {
            if (idx < records.size()) {
                return records[idx++];
            }
            return std::nullopt;
        },
        m_InsAsmDict);
}

void CuInsAssemblerRepos::save2file(const std::string& fileName) {
    CuAsmLogger::logTimeIt("CuInsAssemblerRepos::save2file", [&]() {
        CuAsmLogger::logEntry("Saving to " + fileName + "...");
        std::ofstream fout(fileName);
        if (!fout) {
            throw std::runtime_error("Cannot open " + fileName + " for writing!");
        }
        fout << repr();
    })();
}

std::vector<InsInfo> CuInsAssemblerRepos::iterRecords(const std::function<bool(const std::string&)>& keyFilter) const {
    std::vector<InsInfo> out;
    for (const auto& [key, insAsm] : m_InsAsmDict) {
        if (keyFilter && !keyFilter(key)) {
            continue;
        }
        for (const InsInfo& r : insAsm.iterRecords()) {
            out.push_back(r);
        }
    }
    return out;
}

std::vector<InsFeederRecord> CuInsAssemblerRepos::recordsFeeder(const std::function<bool(const std::string&)>& keyFilter) const {
    std::vector<InsFeederRecord> out;
    for (const InsInfo& r : iterRecords(keyFilter)) {
        out.push_back(InsFeederRecord{r.addr, r.code, r.asmText, 0});
    }
    return out;
}

void CuInsAssemblerRepos::showErrRecords() {
    for (const auto& [insKey, insAsm] : m_InsAsmDict) {
        if (!insAsm.m_ErrRecords.empty()) {
            std::cout << "#### ErrRecords for " << insKey << ":" << std::endl;
        }

        for (const auto& [codeDiff, info] : insAsm.m_ErrRecords) {
            (void)codeDiff;
            std::cout << "  " << hexFixedWidth(info.addr, 8) << " : " << info.asmText << std::endl;
            std::cout << "      Org: " << m_Arch->formatCode(info.code) << std::endl;

            const BigInt acode = assemble(info.addr, info.asmText);
            std::cout << "      Asm: " << m_Arch->formatCode(acode) << std::endl;

            const BigInt diff = acode > info.code ? acode - info.code : info.code - acode;
            std::string diffs = m_Arch->formatCode(diff).substr(2);
            std::replace(diffs.begin(), diffs.end(), '0', ' ');
            std::cout << "     Diff:   " << diffs << std::endl;
        }
    }
}

void CuInsAssemblerRepos::clearErrRecords() {
    for (auto& [key, insAsm] : m_InsAsmDict) {
        (void)key;
        insAsm.m_ErrRecords.clear();
    }
}

std::vector<InsFeederRecord> CuInsAssemblerRepos::genPredRecords() const {
    std::vector<InsFeederRecord> out;
    for (const auto& [key, insAsm] : m_InsAsmDict) {
        (void)key;
        if (insAsm.m_InsRecords.empty()) {
            continue;
        }

        const InsInfo& insInfo = insAsm.m_InsRecords.front();
        const auto pred = m_Arch->genPredCode(insInfo.addr, insInfo.code, insInfo.asmText);
        if (pred.has_value()) {
            const auto& [addr, code, asmText] = *pred;
            out.push_back(InsFeederRecord{addr, code, asmText, 0});
        }
    }
    return out;
}

std::vector<InsFeederRecord> CuInsAssemblerRepos::genUndefRecords() const {
    std::vector<InsFeederRecord> out;
    for (const std::uint64_t v : {std::uint64_t(0x1), std::uint64_t(0x2), std::uint64_t(0x3)}) {
        std::ostringstream asmText;
        asmText << "UNDEF " << std::hex << "0x" << v << ";";
        out.push_back(InsFeederRecord{0x0, BigInt(v), asmText.str(), 0});
    }
    return out;
}

std::string CuInsAssemblerRepos::getInsKeyCandidates(const std::string& key, int n) const {
    std::vector<std::pair<double, std::string>> scored;
    for (const auto& [k, insAsm] : m_InsAsmDict) {
        (void)insAsm;
        const double ratio = similarityRatio(key, k);
        if (ratio >= 0.6) {
            scored.emplace_back(ratio, k);
        }
    }
    std::sort(scored.begin(), scored.end(), [](const auto& x, const auto& y) { return x.first > y.first; });

    if (scored.empty()) {
        return "None";
    }

    std::string out;
    const int limit = std::min<int>(n, static_cast<int>(scored.size()));
    for (int i = 0; i < limit; ++i) {
        out += "        " + scored[static_cast<std::size_t>(i)].second;
        if (i + 1 < limit) {
            out += "\n";
        }
    }
    return out;
}

std::string CuInsAssemblerRepos::repr() const {
    std::ostringstream sio;
    sio << "CuInsAssemblerRepos({";

    std::size_t i = 0;
    for (const auto& [key, insAsm] : m_InsAsmDict) {
        sio << pyStrRepr(key) << ":" << insAsm.repr();
        if (++i < m_InsAsmDict.size()) {
            sio << ",\n";
        }
    }

    sio << "}, arch=";
    if (m_Arch.has_value()) {
        sio << "CuSMVersion(" << m_Arch->getVersionNumber() << ")";
    } else {
        sio << "None";
    }
    sio << ")";

    return sio.str();
}

std::string CuInsAssemblerRepos::toString() const {
    return "CuInsAssemblerRepos(" + std::to_string(m_InsAsmDict.size()) + " keys)";
}

} // namespace CuAsm
