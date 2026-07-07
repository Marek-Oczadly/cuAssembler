#include "CuInsAssembler.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "CuAsmLogger.hpp"

namespace CuAsm {

namespace {

/** @brief Finds a modifier's assigned index in a ModiSet, mirroring dict lookup by key. */
int indexOfModi(const ModiSet& modiSet, const std::string& modi) {
    const auto it = std::find(modiSet.begin(), modiSet.end(), modi);
    return it == modiSet.end() ? -1 : static_cast<int>(it - modiSet.begin());
}

/** @brief Checks whether every modifier in modi is already present in modiSet. */
bool allModiKnown(const ModiSet& modiSet, const InsModi& modi) {
    return std::all_of(modi.begin(), modi.end(), [&](const std::string& m) { return indexOfModi(modiSet, m) >= 0; });
}

/// Backing store for m_ErrRecords; keyed lookups, mirroring dict lookup by key.
using ErrRecords = std::vector<std::pair<BigInt, InsInfo>>;

/** @brief Finds an error record by its code-diff key, mirroring dict lookup by key. */
ErrRecords::iterator findErrRecord(ErrRecords& records, const BigInt& codeDiff) {
    return std::find_if(records.begin(), records.end(), [&codeDiff](const auto& kv) { return kv.first == codeDiff; });
}

/** @brief Inserts or overwrites an error record, mirroring dict `__setitem__` (which preserves the
 *         key's original position on overwrite instead of moving it to the end). */
void setErrRecord(ErrRecords& records, const BigInt& codeDiff, const InsInfo& info) {
    const auto it = findErrRecord(records, codeDiff);
    if (it == records.end()) {
        records.emplace_back(codeDiff, info);
    } else {
        it->second = info;
    }
}

/** @brief python repr() of a plain string, mirroring the single-quoted string literals used
 *         throughout CuInsAssembler.__repr__. */
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

/** @brief python repr() of an int list, e.g. "[7, 1, 0, 32]". */
std::string pyIntListRepr(const InsVals& vals) {
    std::string out = "[";
    for (std::size_t i = 0; i < vals.size(); ++i) {
        out += vals[i].str();
        if (i + 1 < vals.size()) {
            out += ", ";
        }
    }
    out += "]";
    return out;
}

/** @brief python repr() of a string list, e.g. "['0_MOV']". */
std::string pyStrListRepr(const InsModi& modi) {
    std::string out = "[";
    for (std::size_t i = 0; i < modi.size(); ++i) {
        out += pyStrRepr(modi[i]);
        if (i + 1 < modi.size()) {
            out += ", ";
        }
    }
    out += "]";
    return out;
}

/** @brief python '%#x' formatting of a (possibly negative) big integer, e.g. "0x1a3"/"-0x1a3". */
std::string pyHexRepr(const BigInt& v) {
    std::ostringstream oss;
    if (v < 0) {
        oss << "-0x" << std::hex << -v;
    } else {
        oss << "0x" << std::hex << v;
    }
    return oss.str();
}

/** @brief python '%#<width>x' formatting: "0x"-prefixed hex, zero-padded so the whole field
 *         (sign + "0x" + digits) is `width` characters wide. */
std::string pyHexFixedWidth(const BigInt& v, int width) {
    const bool neg = v < 0;
    const BigInt av = neg ? -v : v;
    std::ostringstream hexOss;
    hexOss << std::hex << av;
    const std::string digits = hexOss.str();

    const int prefixLen = (neg ? 1 : 0) + 2;
    const int padLen = std::max(0, width - prefixLen - static_cast<int>(digits.size()));
    return (neg ? std::string("-") : std::string()) + "0x" + std::string(static_cast<std::size_t>(padLen), '0') + digits;
}

/** @brief python '%#0<width>x' formatting of a std::uint64_t (e.g. addr fields). */
std::string hexFixedWidth(std::uint64_t v, int width) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setfill('0') << std::setw(std::max(0, width - 2)) << v;
    return oss.str();
}

/** @brief Renders a RationalMatrix as sympy's default Matrix repr: decimal, column-aligned, no
 *         trailing comma after the last row. Mirrors repr(sympy.Matrix) as used for ValMatrix. */
std::string reprDecimalMat(const RationalMatrix& mat) {
    const int rows = mat.rows();
    const int cols = mat.cols();
    if (rows == 0 || cols == 0) {
        return "Matrix(0, 0, [])";
    }

    std::vector<std::vector<std::string>> smat(static_cast<std::size_t>(rows), std::vector<std::string>(static_cast<std::size_t>(cols)));
    std::vector<std::size_t> colw(static_cast<std::size_t>(cols), 0);

    for (int j = 0; j < cols; ++j) {
        for (int i = 0; i < rows; ++i) {
            const BigRational& val = mat(i, j);
            std::ostringstream cell;
            if (boost::multiprecision::denominator(val) == 1) {
                cell << boost::multiprecision::numerator(val);
            } else {
                cell << val;
            }
            smat[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = cell.str();
            colw[static_cast<std::size_t>(j)] = std::max(colw[static_cast<std::size_t>(j)], cell.str().size());
        }
    }

    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            std::string& s = smat[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            if (s.size() < colw[static_cast<std::size_t>(j)]) {
                s = std::string(colw[static_cast<std::size_t>(j)] - s.size(), ' ') + s;
            }
        }
    }

    std::ostringstream sio;
    sio << "Matrix([\n";
    for (int i = 0; i < rows; ++i) {
        sio << '[';
        for (int j = 0; j < cols; ++j) {
            sio << smat[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            if (j + 1 < cols) {
                sio << ", ";
            }
        }
        sio << ']';
        if (i + 1 < rows) {
            sio << ",\n";
        }
    }
    sio << "])";
    return sio.str();
}

/** @brief Renders a RationalMatrix in the project's reprHexMat style (hex, column-aligned, with
 *         a trailing comma after every row including the last), mirroring common.py/common.hpp's
 *         reprHexMat as used for Rhs. */
std::string reprHexMatBig(const RationalMatrix& mat) {
    const int rows = mat.rows();
    const int cols = mat.cols();
    if (rows == 0 || cols == 0) {
        return "Matrix([\n])";
    }

    std::vector<std::vector<std::string>> smat(static_cast<std::size_t>(rows), std::vector<std::string>(static_cast<std::size_t>(cols)));
    std::vector<std::size_t> colw(static_cast<std::size_t>(cols), 0);

    for (int j = 0; j < cols; ++j) {
        for (int i = 0; i < rows; ++i) {
            const BigRational& val = mat(i, j);
            std::string s;
            if (boost::multiprecision::denominator(val) == 1) {
                s = pyHexRepr(boost::multiprecision::numerator(val));
            } else {
                std::ostringstream oss;
                oss << val;
                s = oss.str();
            }
            smat[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = s;
            colw[static_cast<std::size_t>(j)] = std::max(colw[static_cast<std::size_t>(j)], s.size());
        }
    }

    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            std::string& s = smat[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            if (s.size() < colw[static_cast<std::size_t>(j)]) {
                s = std::string(colw[static_cast<std::size_t>(j)] - s.size(), ' ') + s;
            }
        }
    }

    std::ostringstream sio;
    sio << "Matrix([\n";
    for (int i = 0; i < rows; ++i) {
        sio << '[';
        for (int j = 0; j < cols; ++j) {
            sio << smat[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            if (j + 1 < cols) {
                sio << ", ";
            }
        }
        sio << "],\n";
    }
    sio << "])";
    return sio.str();
}

/** @brief Floor division on exact rationals (rounds toward -infinity), mirroring python's `//`. */
BigInt floorDiv(const BigRational& r) {
    const BigInt num = boost::multiprecision::numerator(r);
    const BigInt den = boost::multiprecision::denominator(r); // always > 0 for a canonical cpp_rational
    BigInt q = num / den;
    const BigInt rem = num % den;
    if (rem != 0 && num < 0) {
        --q;
    }
    return q;
}

} // namespace

CuInsAssembler::CuInsAssembler(const std::string& insKey, const std::string& arch) : m_InsKey(insKey), m_Arch(arch) {}

CuInsAssembler::CuInsAssembler(const CuInsAssemblerState& state) : m_Arch(state.arch) {
    initFromDict(state);
}

void CuInsAssembler::initFromDict(const CuInsAssemblerState& state) {
    m_InsKey = state.insKey;
    m_InsRepos = state.insRepos;
    m_InsModiSet = state.insModiSet;

    m_ValMatrix = state.valMatrix;
    m_PSol = state.pSol;
    m_PSolFac = state.pSolFac;
    m_ValNullMat = state.valNullMat;
    m_Rhs = state.rhs;

    m_InsRecords = state.insRecords;
    m_ErrRecords = state.errRecords;

    m_Arch = state.arch;
}

void CuInsAssembler::initFromJsonDict() {
    // Intentionally empty, mirroring CuInsAssembler.initFromJsonDict's `pass` body.
}

std::vector<InsInfo> CuInsAssembler::iterRecords() const {
    std::vector<InsInfo> result = m_InsRecords;
    for (const auto& [codeDiff, info] : m_ErrRecords) {
        (void)codeDiff;
        result.push_back(info);
    }
    return result;
}

std::vector<InsFeederRecord> CuInsAssembler::recordsFeeder() const {
    std::vector<InsFeederRecord> result;
    for (const InsInfo& r : iterRecords()) {
        result.push_back(InsFeederRecord{r.addr, r.code, r.asmText, 0});
    }
    return result;
}

bool CuInsAssembler::expandModiSet(const InsModi& modi) {
    bool updated = false;
    for (const std::string& m : modi) {
        if (indexOfModi(m_InsModiSet, m) < 0) {
            m_InsModiSet.push_back(m);
            updated = true;
        }
    }
    return updated;
}

std::vector<BigRational> CuInsAssembler::buildInsValVec(const InsVals& vals, const InsModi& modi) const {
    std::vector<BigRational> insval;
    insval.reserve(m_InsModiSet.size() + vals.size());
    for (const std::string& m : m_InsModiSet) {
        const bool has = std::find(modi.begin(), modi.end(), m) != modi.end();
        insval.emplace_back(has ? 1 : 0);
    }
    for (const BigInt& v : vals) {
        insval.emplace_back(v);
    }
    return insval;
}

RationalMatrix CuInsAssembler::buildInsValVecMatrix(const InsVals& vals, const InsModi& modi) const {
    return RationalMatrix::columnVector(buildInsValVec(vals, modi));
}

CuInsAssembler::AssembleCheck CuInsAssembler::canAssemble(const InsVals& vals, const InsModi& modi) const {
    if (!allModiKnown(m_InsModiSet, modi)) {
        std::string unknown;
        for (const std::string& m : modi) {
            if (indexOfModi(m_InsModiSet, m) < 0) {
                if (!unknown.empty()) {
                    unknown += ", ";
                }
                unknown += pyStrRepr(m);
            }
        }
        return {"NewModi", "Unknown modifiers: ({" + unknown + "})"};
    }

    const RationalMatrix insvec = buildInsValVecMatrix(vals, modi);
    if (m_ValNullMat.rows() != 0) {
        const RationalMatrix insrhs = m_ValNullMat * insvec;
        if (!insrhs.isZero()) {
            return {"NewVals", "Insufficient basis, try CuAsming more instructions!"};
        }
    }

    return {std::nullopt, ""};
}

CuInsAssembler::PushResult CuInsAssembler::push(const InsVals& vals, const InsModi& modi, const BigInt& code, const InsInfo& insInfo) {
    if (!allModiKnown(m_InsModiSet, modi)) {
        CuAsmLogger::logProcedure("Pushing new modi (" + m_Arch.formatCode(code) + ", " + m_InsKey + "): " + insInfo.asmText);
        expandModiSet(modi);
        m_InsRepos.push_back(InsReposEntry{vals, modi, code});
        buildMatrix();
        m_InsRecords.push_back(insInfo);
        return {true, "NewModi"};
    }

    const RationalMatrix insvec = buildInsValVecMatrix(vals, modi);

    bool doVerify;
    if (m_ValNullMat.rows() == 0) {
        // Mirrors `self.m_ValNullMat is None`; also covers the "never built yet" state, which is
        // functionally equivalent (no null-space constraints exist to check against).
        doVerify = true;
    } else {
        const RationalMatrix insrhs = m_ValNullMat * insvec;
        doVerify = insrhs.isZero();
    }

    if (!doVerify) {
        CuAsmLogger::logProcedure("Pushing new vals (" + m_Arch.formatCode(code) + ", " + m_InsKey + "): " + insInfo.asmText);
        m_InsRepos.push_back(InsReposEntry{vals, modi, code});
        m_InsRecords.push_back(insInfo);
        buildMatrix();
        return {true, "NewVals"};
    }

    const BigRational inscodeRat = m_PSol.dot(insvec) / BigRational(m_PSolFac);
    const BigRational codeRat(code);

    if (inscodeRat == codeRat) {
        return {true, "Verified"};
    }

    if (boost::multiprecision::denominator(inscodeRat) == 1) {
        const BigInt inscode = boost::multiprecision::numerator(inscodeRat);
        const BigInt codeDiff = inscode - code;

        const auto it = findErrRecord(m_ErrRecords, codeDiff);
        if (it == m_ErrRecords.end()) {
            setErrRecord(m_ErrRecords, codeDiff, insInfo);
            CuAsmLogger::logError("Error when verifying for " + m_InsKey);
            CuAsmLogger::logError("    Asm : " + insInfo.asmText);
            CuAsmLogger::logError("    InputCode : " + m_Arch.formatCode(code));
            CuAsmLogger::logError("    AsmCode   : " + m_Arch.formatCode(inscode));
            return {false, "NewConflict"};
        }

        CuAsmLogger::logDebug("Known code conflict for " + m_InsKey + "!");
        return {false, "KnownConflict"};
    }

    CuAsmLogger::logCritical("FATAL! Non-integral code assembled for " + m_InsKey);
    CuAsmLogger::logCritical("    Asm : " + insInfo.asmText);
    CuAsmLogger::logCritical("    InputCode : " + m_Arch.formatCode(code));
    std::ostringstream oss;
    oss << inscodeRat;
    CuAsmLogger::logCritical("    AsmCode   : (" + oss.str() + ")!");

    setErrRecord(m_ErrRecords, code, insInfo);
    return {false, "NewConflict"};
}

BigInt CuInsAssembler::buildCode(const InsVals& vals, const InsModi& modi) const {
    BigRational inscode(0);

    const int total = m_PSol.rows();
    const int nVals = static_cast<int>(vals.size());
    const int valStart = total - nVals;
    for (int i = 0; i < nVals; ++i) {
        inscode += m_PSol(valStart + i, 0) * BigRational(vals[static_cast<std::size_t>(i)]);
    }

    for (const std::string& m : modi) {
        const int idx = indexOfModi(m_InsModiSet, m);
        inscode += m_PSol(idx, 0);
    }

    if (m_PSolFac == 1) {
        return floorDiv(inscode);
    }
    return floorDiv(inscode / BigRational(m_PSolFac));
}

void CuInsAssembler::buildMatrix() {
    if (m_InsRepos.empty()) {
        return;
    }

    std::vector<std::vector<BigRational>> M;
    std::vector<BigRational> b;
    M.reserve(m_InsRepos.size());
    b.reserve(m_InsRepos.size());
    for (const InsReposEntry& entry : m_InsRepos) {
        M.push_back(buildInsValVec(entry.vals, entry.modi));
        b.emplace_back(entry.code);
    }

    m_ValMatrix = RationalMatrix::fromRows(M);
    m_Rhs = RationalMatrix::columnVector(b);
    m_ValNullMat = nullSpaceMatrixOf(m_ValMatrix);

    RationalMatrix lhs = m_ValMatrix;
    RationalMatrix rhs = m_Rhs;
    if (m_ValNullMat.rows() != 0) {
        lhs = RationalMatrix::vstack(m_ValNullMat, m_ValMatrix);
        rhs = RationalMatrix::vstack(RationalMatrix(m_ValNullMat.rows(), 1, BigRational(0)), m_Rhs);
    }

    const RationalMatrix rawSol = lhs.solve(rhs);
    const auto [scaled, fac] = scaleToIntegerMatrix(rawSol);
    m_PSol = scaled;
    m_PSolFac = fac;
}

std::optional<RationalMatrix> CuInsAssembler::solve() const {
    if (m_ValNullMat.rows() != 0) {
        std::cout << "Not solvable!" << std::endl;
        return std::nullopt;
    }

    const RationalMatrix x = m_ValMatrix.solve(m_Rhs);
    std::cout << "Solution: " << std::endl;
    for (int i = 0; i < x.rows(); ++i) {
        const BigInt v = boost::multiprecision::numerator(x(i, 0));
        std::cout << i << " : " << pyHexFixedWidth(v, 33) << std::endl;
    }
    return x;
}

void CuInsAssembler::printSolution() const {
    std::cout << "InsKey = " << m_InsKey << std::endl;
    if (m_InsRepos.empty()) {
        return;
    }

    const int nvals = static_cast<int>(m_InsRepos[0].vals.size());
    const int nmodi = static_cast<int>(m_InsModiSet.size());

    std::vector<std::string> names(static_cast<std::size_t>(nvals + nmodi));
    for (int v = 0; v < nvals; ++v) {
        names[static_cast<std::size_t>(v)] = "V" + std::to_string(v);
    }
    for (int i = 0; i < nmodi; ++i) {
        names[static_cast<std::size_t>(nvals + i)] = m_InsModiSet[static_cast<std::size_t>(i)];
    }

    std::vector<BigRational> revSol;
    for (int i = nmodi; i < m_PSol.rows(); ++i) {
        revSol.push_back(m_PSol(i, 0));
    }
    for (int i = 0; i < nmodi; ++i) {
        revSol.push_back(m_PSol(i, 0));
    }

    const std::size_t n = std::min(names.size(), revSol.size());
    for (std::size_t i = 0; i < n; ++i) {
        const BigInt val = boost::multiprecision::numerator(revSol[i]);
        if (val % m_PSolFac == 0) {
            std::cout << "  " << std::setw(24) << names[i] << " :  " << pyHexFixedWidth(val / m_PSolFac, 32) << std::endl;
        } else {
            std::cout << "  " << std::setw(24) << names[i] << " :  " << pyHexFixedWidth(val, 32) << " / " << pyHexRepr(m_PSolFac)
                       << " " << std::endl;
        }
    }
}

std::string CuInsAssembler::reprPSol() const {
    const int nvals = m_InsRepos.empty() ? 0 : static_cast<int>(m_InsRepos[0].vals.size());
    const int nmodi = static_cast<int>(m_InsModiSet.size());
    const int total = nvals + nmodi;

    std::vector<std::string> names(static_cast<std::size_t>(nmodi));
    for (int i = 0; i < nmodi; ++i) {
        names[static_cast<std::size_t>(i)] = m_InsModiSet[static_cast<std::size_t>(i)];
    }
    names.emplace_back("Pred");
    for (int v = 1; v < nvals; ++v) {
        names.push_back("V" + std::to_string(v));
    }

    std::vector<std::string> slist;
    std::vector<BigRational> vlist;
    std::size_t maxvlen = 0;
    for (int ival = 0; ival < total; ++ival) {
        const BigRational val = m_PSol(ival, 0);
        const std::string sval = pyHexRepr(boost::multiprecision::numerator(val));
        slist.push_back(sval);
        vlist.push_back(val);
        maxvlen = std::max(maxvlen, sval.size());
    }

    std::size_t maxnlen = 0;
    for (const std::string& n : names) {
        maxnlen = std::max(maxnlen, n.size());
    }

    std::ostringstream sio;
    sio << "Matrix([\n";
    if (m_PSolFac == 1) {
        for (int i = 0; i < total; ++i) {
            const std::string ss = std::string(maxvlen - slist[static_cast<std::size_t>(i)].size(), ' ') + slist[static_cast<std::size_t>(i)];
            sio << "[ " << ss << "], # " << names[static_cast<std::size_t>(i)] << "\n";
        }
    } else {
        for (int i = 0; i < total; ++i) {
            const std::string ss = std::string(maxvlen - slist[static_cast<std::size_t>(i)].size(), ' ') + slist[static_cast<std::size_t>(i)];
            const std::string ns =
                std::string(maxnlen - names[static_cast<std::size_t>(i)].size(), ' ') + names[static_cast<std::size_t>(i)];
            const BigInt vv = boost::multiprecision::numerator(vlist[static_cast<std::size_t>(i)]);
            if (vv % m_PSolFac == 0) {
                sio << "[ " << ss << "], # " << ns << " : " << pyHexFixedWidth(vv / m_PSolFac, 32) << "\n";
            } else {
                sio << "[ " << ss << "], # " << ns << " : " << pyHexFixedWidth(vv, 32) << " / " << pyHexRepr(m_PSolFac) << "\n";
            }
        }
    }
    sio << "])";
    return sio.str();
}

std::string CuInsAssembler::repr() const {
    std::ostringstream sio;
    sio << "CuInsAssembler(\"\", {\"InsKey\" : " << pyStrRepr(m_InsKey) << ", \n";

    sio << "  \"InsRepos\" : [";
    for (std::size_t i = 0; i < m_InsRepos.size(); ++i) {
        const InsReposEntry& e = m_InsRepos[i];
        sio << "(" << pyIntListRepr(e.vals) << ", " << pyStrListRepr(e.modi) << ", " << e.code.str() << ")";
        if (i + 1 < m_InsRepos.size()) {
            sio << ",\n";
        }
    }
    sio << "], \n";

    sio << "  \"InsModiSet\" : {";
    for (std::size_t i = 0; i < m_InsModiSet.size(); ++i) {
        sio << pyStrRepr(m_InsModiSet[i]) << ": " << i;
        if (i + 1 < m_InsModiSet.size()) {
            sio << ", ";
        }
    }
    sio << "}, \n";

    sio << "  \"ValMatrix\" : " << reprDecimalMat(m_ValMatrix) << ", \n";
    sio << "  \"PSol\" : " << reprPSol() << ", \n";
    sio << "  \"PSolFac\" : " << m_PSolFac.str() << ", \n";

    if (m_ValNullMat.rows() == 0) {
        sio << "  \"ValNullMat\" : None, \n";
    } else {
        sio << "  \"ValNullMat\" : " << reprDecimalMat(m_ValNullMat) << ", \n";
    }

    sio << "  \"InsRecords\" : [";
    for (const InsInfo& r : m_InsRecords) {
        sio << "(" << hexFixedWidth(r.addr, 8) << ", " << m_Arch.formatCode(r.code) << ", \"" << r.asmText << "\"),\n";
    }
    sio << "], \n";

    sio << "  \"ErrRecords\" : {";
    for (const auto& [codeDiff, r] : m_ErrRecords) {
        sio << pyHexRepr(codeDiff) << " : (" << hexFixedWidth(r.addr, 8) << ", " << m_Arch.formatCode(r.code) << ", \"" << r.asmText
            << "\"),\n";
    }
    sio << "}, ";

    sio << "  \"Rhs\" : " << reprHexMatBig(m_Rhs) << ", \n";
    sio << "  \"Arch\" : CuSMVersion(" << m_Arch.getVersionNumber() << ") })";

    return sio.str();
}

std::string CuInsAssembler::toString() const {
    return "CuInsAssembler(" + m_InsKey + ")";
}

} // namespace CuAsm
