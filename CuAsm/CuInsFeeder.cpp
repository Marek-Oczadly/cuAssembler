#include "CuInsFeeder.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "CuAsmLogger.hpp"
#include "CuControlCode.hpp"
#include "common.hpp"

namespace CuAsm {

namespace {

/** @brief Zero-pads a line number to 4 digits, mirroring python's '%04d'. */
std::string zeroPad4(std::uint64_t v) {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << v;
    return oss.str();
}

// Patterns mirroring SassLineType's regexes, tried in this exact order (the last always matches,
// as a catch-all "Others" fallback). Named groups aren't available in std::regex, so plain capture
// groups are used instead, with indices commented per pattern.
const std::regex& insCodeRe() {
    static const std::regex re(R"(^\s*/\*(\w+)\*/\s*\{?\s*(.*;)\s*/\* (.*) \*/)"); // 1=addr 2=asm 3=code
    return re;
}
const std::regex& insOnlyRe() {
    static const std::regex re(R"(^\s*/\*(\w+)\*/\s*(.*\}))"); // 1=addr 2=asm
    return re;
}
const std::regex& codeOnlyRe() {
    static const std::regex re(R"(^\s*/\* (0x[0-9a-f]{16}) \*/)"); // 1=code
    return re;
}
const std::regex& funcNameRe() {
    static const std::regex re(R"(^\s*Function\s*:\s*(.*))"); // 1=func
    return re;
}
const std::regex& sectionNameRe() {
    static const std::regex re(R"(^\s*\.section\s*\.text\.([^,\s]*))"); // 1=sec
    return re;
}
const std::regex& headerFlagRe() {
    static const std::regex re(R"(^\s*\.headerflags.*EF_CUDA_SM(\d+))"); // 1=arch digits
    return re;
}

} // namespace

void CuInsFeeder::initFilters(const std::string& archFilter, const std::string& insFilter) {
    m_InsFilterFun = makeFilterFun(insFilter);
    if (!archFilter.empty()) {
        m_ArchFilter = CuSMVersion(archFilter);
    }
}

std::function<bool(const std::string&)> CuInsFeeder::makeFilterFun(const std::string& pattern) {
    if (pattern.empty()) {
        return [](const std::string&) { return true; };
    }
    const std::regex re(pattern);
    return [re](const std::string& s) { return std::regex_search(s, re); };
}

CuInsFeeder::CuInsFeeder(const std::string& fileName, const std::string& archFilter, const std::string& insFilter)
    : m_FileName(fileName) {
    m_OwnedFile.open(fileName);
    if (!m_OwnedFile) {
        throw std::runtime_error("Cannot open " + fileName + " for reading!");
    }
    m_Stream = &m_OwnedFile;
    initFilters(archFilter, insFilter);
}

CuInsFeeder::CuInsFeeder(std::istream& stream, const std::string& archFilter, const std::string& insFilter)
    : m_Stream(&stream) {
    initFilters(archFilter, insFilter);
}

CuInsFeeder::ParsedLine CuInsFeeder::matchLineType(const std::string& line) {
    std::smatch m;
    ParsedLine pl;
    pl.text = line;

    if (std::regex_search(line, m, insCodeRe())) {
        pl.type = LineType::InsCode;
        pl.addr = std::stoull(m[1].str(), nullptr, 16);
        pl.asmText = m[2].str();
        pl.code = BigInt(m[3].str());
        return pl;
    }
    if (std::regex_search(line, m, insOnlyRe())) {
        pl.type = LineType::InsOnly;
        pl.addr = std::stoull(m[1].str(), nullptr, 16);
        pl.asmText = m[2].str();
        return pl;
    }
    if (std::regex_search(line, m, codeOnlyRe())) {
        pl.type = LineType::CodeOnly;
        pl.code = BigInt(m[1].str());
        return pl;
    }
    if (std::regex_search(line, m, funcNameRe())) {
        pl.type = LineType::FuncName;
        pl.func = m[1].str();
        return pl;
    }
    if (std::regex_search(line, m, sectionNameRe())) {
        pl.type = LineType::SectionName;
        pl.sec = m[1].str();
        return pl;
    }
    if (std::regex_search(line, m, headerFlagRe())) {
        pl.type = LineType::HeaderFlag;
        pl.archDigits = m[1].str();
        return pl;
    }

    pl.type = LineType::Others;
    return pl;
}

std::optional<CuInsFeeder::ParsedLine> CuInsFeeder::nextParseLine() {
    std::string line;
    ++m_LineNo;
    if (!std::getline(*m_Stream, line)) {
        return std::nullopt;
    }
    return matchLineType(line);
}

void CuInsFeeder::setFuncName(const std::string& func) {
    CurrFuncName = func;
}

void CuInsFeeder::setSectionName(const std::string& sec) {
    // Section name is ".text.<funcname>"; mirrors __setSectionName reusing CurrFuncName.
    CurrFuncName = sec;
}

void CuInsFeeder::switchArch(const std::string& archDigits) {
    const CuSMVersion smVersion(archDigits);
    m_CurrArchVersion = smVersion;
    CurrArch = smVersion.getVersionString();

    const int major = smVersion.getMajor();
    if (major == 3) {
        m_ArchFamily = ArchFamily::Sm3x;
    } else if (major == 5 || major == 6) {
        m_ArchFamily = ArchFamily::Sm5x6x;
    } else if (major == 7 || major == 8) {
        m_ArchFamily = ArchFamily::Sm7x8x;
    } else {
        throw std::runtime_error("ERROR! No implemented state machine for arch " + smVersion.getVersionString() + "!!!");
    }
}

void CuInsFeeder::emitMessage(const std::string& msg) const {
    CuAsmLogger::logWarning("CuInsFeeder Message: " + msg + " @Line" + zeroPad4(m_LineNo - 1));
}

void CuInsFeeder::pushAddr(std::uint64_t addr) {
    m_AddrList.push_back(addr);
}

void CuInsFeeder::pushAsm(const std::string& asmText) {
    m_AsmList.push_back(asmText);
}

void CuInsFeeder::pushCode(const BigInt& code) {
    m_CodeList.push_back(code);
}

void CuInsFeeder::pushInsCode(std::uint64_t addr, const std::string& asmText, const BigInt& code) {
    pushAddr(addr);
    pushAsm(asmText);
    pushCode(code);
}

void CuInsFeeder::pushInsOnly(std::uint64_t addr, const std::string& asmText) {
    pushAddr(addr);
    pushAsm(asmText);
}

void CuInsFeeder::throwInvalidOp(LineType op) const {
    static const char* const kNames[] = {"InsCode", "InsOnly", "CodeOnly", "FuncName", "SectionName", "HeaderFlag", "Others"};
    throw std::runtime_error(std::string("Invalid input ") + kNames[static_cast<int>(op)] + " (Unacceptable op) @Line" +
                              std::to_string(m_LineNo) + "!!!");
}

CuInsFeeder::ParserState CuInsFeeder::feedDefault(const ParsedLine& pl) {
    switch (pl.type) {
        case LineType::FuncName: setFuncName(pl.func); return ParserState::Ready;
        case LineType::SectionName: setSectionName(pl.sec); return ParserState::Ready;
        case LineType::HeaderFlag: switchArch(pl.archDigits); return ParserState::Ready;
        case LineType::Others: return ParserState::Ready;
        default: throwInvalidOp(pl.type);
    }
}

CuInsFeeder::ParserState CuInsFeeder::feedSm3x(const ParsedLine& pl) {
    switch (pl.type) {
        case LineType::FuncName: setFuncName(pl.func); return ParserState::Ready;
        case LineType::SectionName: setSectionName(pl.sec); return ParserState::Ready;
        case LineType::HeaderFlag: switchArch(pl.archDigits); return ParserState::Ready;
        case LineType::CodeOnly: return ParserState::Ready;
        case LineType::InsCode: pushInsCode(pl.addr, pl.asmText, pl.code); return ParserState::Ready;
        case LineType::Others: return ParserState::Ready;
        default: throwInvalidOp(pl.type);
    }
}

CuInsFeeder::ParserState CuInsFeeder::feedSm5x6x(const ParsedLine& pl) {
    switch (m_PState) {
        case ParserState::Ready:
            switch (pl.type) {
                case LineType::FuncName: setFuncName(pl.func); return ParserState::Ready;
                case LineType::SectionName: setSectionName(pl.sec); return ParserState::Ready;
                case LineType::HeaderFlag: switchArch(pl.archDigits); return ParserState::Ready;
                case LineType::CodeOnly: pushCode(pl.code); return ParserState::WaitForIns3;
                case LineType::Others: return ParserState::Ready;
                default: throwInvalidOp(pl.type);
            }
        case ParserState::WaitForIns3:
            if (pl.type == LineType::InsCode) {
                pushInsCode(pl.addr, pl.asmText, pl.code);
                return ParserState::WaitForIns2;
            }
            if (pl.type == LineType::InsOnly) {
                pushInsOnly(pl.addr, pl.asmText);
                return ParserState::WaitForCode3;
            }
            throwInvalidOp(pl.type);
        case ParserState::WaitForCode3:
            if (pl.type == LineType::CodeOnly) {
                pushCode(pl.code);
                return ParserState::WaitForIns2;
            }
            throwInvalidOp(pl.type);
        case ParserState::WaitForIns2:
            if (pl.type == LineType::InsCode) {
                pushInsCode(pl.addr, pl.asmText, pl.code);
                return ParserState::WaitForIns1;
            }
            if (pl.type == LineType::InsOnly) {
                pushInsOnly(pl.addr, pl.asmText);
                return ParserState::WaitForCode2;
            }
            throwInvalidOp(pl.type);
        case ParserState::WaitForCode2:
            if (pl.type == LineType::CodeOnly) {
                pushCode(pl.code);
                return ParserState::WaitForIns1;
            }
            throwInvalidOp(pl.type);
        case ParserState::WaitForIns1:
            if (pl.type == LineType::InsCode) {
                pushInsCode(pl.addr, pl.asmText, pl.code);
                return ParserState::Ready;
            }
            if (pl.type == LineType::InsOnly) {
                pushInsOnly(pl.addr, pl.asmText);
                return ParserState::WaitForCode1;
            }
            throwInvalidOp(pl.type);
        case ParserState::WaitForCode1:
            if (pl.type == LineType::CodeOnly) {
                pushCode(pl.code);
                return ParserState::Ready;
            }
            throwInvalidOp(pl.type);
    }
    throwInvalidOp(pl.type);
}

CuInsFeeder::ParserState CuInsFeeder::feedSm7x8x(const ParsedLine& pl) {
    switch (m_PState) {
        case ParserState::Ready:
            switch (pl.type) {
                case LineType::FuncName: setFuncName(pl.func); return ParserState::Ready;
                case LineType::SectionName: setSectionName(pl.sec); return ParserState::Ready;
                case LineType::HeaderFlag: switchArch(pl.archDigits); return ParserState::Ready;
                case LineType::InsCode: pushInsCode(pl.addr, pl.asmText, pl.code); return ParserState::WaitForCode1;
                case LineType::CodeOnly: emitMessage("Missing Instruction"); return ParserState::Ready;
                case LineType::Others: return ParserState::Ready;
                default: throwInvalidOp(pl.type);
            }
        case ParserState::WaitForCode1:
            if (pl.type == LineType::CodeOnly) {
                pushCode(pl.code);
                return ParserState::Ready;
            }
            throwInvalidOp(pl.type);
        default:
            throwInvalidOp(pl.type);
    }
}

CuInsFeeder::ParserState CuInsFeeder::feedLineOp(const ParsedLine& pl) {
    ParserState result;
    switch (m_ArchFamily) {
        case ArchFamily::Default: result = feedDefault(pl); break;
        case ArchFamily::Sm3x: result = feedSm3x(pl); break;
        case ArchFamily::Sm5x6x: result = feedSm5x6x(pl); break;
        case ArchFamily::Sm7x8x: result = feedSm7x8x(pl); break;
        default: throwInvalidOp(pl.type);
    }
    m_PState = result;
    return result;
}

std::vector<CuInsRecord> CuInsFeeder::iterPopIns() {
    std::vector<std::uint32_t> ctrlList;
    std::vector<BigInt> codeList;

    switch (m_ArchFamily) {
        case ArchFamily::Sm3x: {
            ctrlList.assign(m_CodeList.size(), 0);
            codeList = m_CodeList;
            break;
        }
        case ArchFamily::Sm5x6x: {
            auto [ctrl, code] = CuSMVersion::splitCtrlCodeFromIntList_5x_6x(m_CodeList);
            ctrlList = std::move(ctrl);
            codeList = std::move(code);
            break;
        }
        case ArchFamily::Sm7x8x: {
            std::vector<BigInt> combined;
            combined.reserve(m_CodeList.size() / 2);
            for (std::size_t i = 0; i + 1 < m_CodeList.size(); i += 2) {
                combined.push_back(m_CodeList[i] + (m_CodeList[i + 1] << 64));
            }
            auto [ctrl, code] = CuSMVersion::splitCtrlCodeFromIntList_7x_8x(combined);
            ctrlList = std::move(ctrl);
            codeList = std::move(code);
            break;
        }
        case ArchFamily::Default:
            break; // No InsCode/CodeOnly transitions are defined before an arch is established.
    }

    const std::size_t n = std::min({m_AddrList.size(), m_AsmList.size(), codeList.size(), ctrlList.size()});
    std::vector<CuInsRecord> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(CuInsRecord{m_AddrList[i], codeList[i], m_AsmList[i], ctrlList[i]});
    }

    m_AddrList.clear();
    m_AsmList.clear();
    m_CodeList.clear();

    return out;
}

bool CuInsFeeder::filterIns(const std::string& asmText) const {
    return !m_InsFilterFun || m_InsFilterFun(asmText);
}

std::string CuInsFeeder::formatCtrlCodeString(std::uint32_t ccode, bool phantomMode) const {
    if (!m_CurrArchVersion.has_value() || m_CurrArchVersion->getMajor() < 5) {
        return "";
    }
    if (phantomMode) {
        return std::string(static_cast<std::size_t>(c_ControlStringLen) + 2, ' ');
    }
    return "[" + CuControlCode::decode(ccode) + "]";
}

std::optional<CuInsRecord> CuInsFeeder::next() {
    if (!m_IterationStarted) {
        restart();
        m_IterationStarted = true;
    }

    while (true) {
        if (m_PendingIdx < m_PendingRecords.size()) {
            return m_PendingRecords[m_PendingIdx++];
        }
        m_PendingRecords.clear();
        m_PendingIdx = 0;

        const std::optional<ParsedLine> pl = nextParseLine();
        if (!pl.has_value()) {
            m_IterationStarted = false;
            return std::nullopt;
        }

        if (pl->type == LineType::HeaderFlag) {
            feedLineOp(*pl);
            m_DoFeed = !m_ArchFilter.has_value() || (m_CurrArchVersion.has_value() && *m_CurrArchVersion == *m_ArchFilter);
            continue;
        }
        if (pl->type == LineType::FuncName || pl->type == LineType::SectionName) {
            feedLineOp(*pl);
            continue;
        }

        if (!m_DoFeed) {
            continue;
        }

        const ParserState ns = feedLineOp(*pl);
        if (ns == ParserState::Ready) {
            for (CuInsRecord& rec : iterPopIns()) {
                if (filterIns(rec.asmText)) {
                    m_PendingRecords.push_back(rec);
                }
            }
        }
    }
}

void CuInsFeeder::trans(const std::string& outFileName, const std::string& codeOnlyLineMode) {
    std::ofstream fout(outFileName);
    if (!fout) {
        throw std::runtime_error("Cannot open " + outFileName + " for writing!");
    }
    trans(fout, codeOnlyLineMode);
}

void CuInsFeeder::trans(std::ostream& outStream, const std::string& codeOnlyLineMode) {
    if (codeOnlyLineMode != "keep" && codeOnlyLineMode != "none") {
        throw std::invalid_argument("Unknown codeOnlyLineMode \"" + codeOnlyLineMode + "\"!");
    }

    restart();

    struct BufferedLine {
        std::string text;
        LineType type;
    };
    struct OutLine {
        std::string text;
        bool isPending = false;
        std::string pendingCode;
    };

    std::vector<BufferedLine> lineBuffers;
    std::vector<OutLine> outBuffers;

    while (true) {
        const std::optional<ParsedLine> pl = nextParseLine();
        if (!pl.has_value()) {
            break;
        }

        const std::string line = rtrim(pl->text);
        lineBuffers.push_back({line, pl->type});

        const ParserState ns = feedLineOp(*pl);

        if (ns == ParserState::Ready) {
            std::vector<CuInsRecord> insList = iterPopIns();
            std::size_t insIdx = 0;

            std::optional<LineType> preLt;
            int lineLen = -1;

            for (const BufferedLine& bl : lineBuffers) {
                if (bl.type == LineType::InsCode) {
                    const std::uint32_t ctrl = insList[insIdx++].ctrl;
                    const std::string ctrlStr = formatCtrlCodeString(ctrl);
                    const std::string outLine = ctrlStr + "  " + bl.text;
                    outBuffers.push_back({outLine + "\n", false, ""});
                    if (lineLen == -1) {
                        lineLen = static_cast<int>(outLine.size());
                    }
                } else if (bl.type == LineType::InsOnly) {
                    const std::uint32_t ctrl = insList[insIdx++].ctrl;
                    const std::string ctrlStr = formatCtrlCodeString(ctrl);
                    outBuffers.push_back({ctrlStr + "  " + bl.text, false, ""});
                } else if (bl.type == LineType::CodeOnly) {
                    if (preLt.has_value() && *preLt == LineType::InsOnly) {
                        OutLine& last = outBuffers.back();
                        last.isPending = true;
                        last.pendingCode = trim(bl.text);
                    } else {
                        const std::string ctrlStr = formatCtrlCodeString(0, true);
                        if (codeOnlyLineMode == "keep") {
                            outBuffers.push_back({ctrlStr + "  " + bl.text + "\n", false, ""});
                        }
                        // "none" (default): skip the code-only line entirely.
                    }
                } else {
                    outBuffers.push_back({bl.text + "\n", false, ""});
                }

                preLt = bl.type;
            }

            for (const OutLine& ol : outBuffers) {
                if (ol.isPending) {
                    const int slen = lineLen - static_cast<int>(ol.text.size()) - static_cast<int>(ol.pendingCode.size());
                    outStream << ol.text << std::string(static_cast<std::size_t>(std::max(0, slen)), ' ') << ol.pendingCode << "\n";
                } else {
                    outStream << ol.text;
                }
            }

            lineBuffers.clear();
            outBuffers.clear();
        }
    }
}

void CuInsFeeder::extract(const std::string& outFileName, std::optional<std::string> funcFilter, std::optional<std::string> insFilter) {
    const std::function<bool(const std::string&)> insFilterFun = makeFilterFun(insFilter.value_or(""));
    const std::function<bool(const std::string&)> funcFilterFun = makeFilterFun(funcFilter.value_or(""));

    std::ostringstream buf;
    bool doDump = false;

    const auto tryDump = [&]() -> bool {
        if (!doDump) {
            return false;
        }
        if (buf.str().empty()) {
            std::cout << "Empty buffer! Nothing to dump..." << std::endl;
            return false;
        }
        std::cout << "================================" << std::endl;
        std::cout << buf.str() << std::endl;

        std::ofstream foutStream(outFileName);
        std::cout << "Dump to file " << outFileName << "..." << std::endl;
        foutStream << buf.str();
        return true;
    };

    while (true) {
        const std::optional<ParsedLine> pl = nextParseLine();
        if (!pl.has_value()) {
            tryDump();
            break;
        }

        if (pl->type == LineType::FuncName) {
            if (tryDump()) {
                break;
            }
            if (funcFilter.has_value() && funcFilterFun(pl->func)) {
                doDump = true;
            }
            buf.str("");
            buf.clear();
        } else if (pl->type == LineType::InsCode || pl->type == LineType::InsOnly || pl->type == LineType::CodeOnly) {
            if (insFilterFun(pl->text)) {
                doDump = true;
            }
        }

        buf << rtrim(pl->text) << "\n";
    }

    if (!doDump) {
        std::cout << "Nothing to dump..." << std::endl;
    }
}

void CuInsFeeder::restart() {
    m_Stream->clear();
    m_Stream->seekg(0);
    if (!m_Stream->good()) {
        throw std::runtime_error("This feeder cannot be restarted!");
    }
    m_LineNo = 0;
    m_DoFeed = false;
    m_PendingRecords.clear();
    m_PendingIdx = 0;
    m_IterationStarted = true;
}

bool CuInsFeeder::close() {
    auto* file = dynamic_cast<std::ifstream*>(m_Stream);
    if (file == nullptr || !file->is_open()) {
        return false;
    }
    file->close();
    m_LineNo = 0;
    return true;
}

std::streampos CuInsFeeder::tell() const {
    return m_Stream->tellg();
}

std::uint64_t CuInsFeeder::tellLine() const {
    return m_LineNo;
}

} // namespace CuAsm
