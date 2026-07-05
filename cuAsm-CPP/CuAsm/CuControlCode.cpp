#include "CuControlCode.hpp"

#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace CuAsm {

namespace {

/** @brief Splits a string on a delimiter character, mirroring python's str.split(sep). */
std::vector<std::string> splitString(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == delim) {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

} // namespace

bool matchesControlCodePattern(const std::string& s) {
    static const std::regex pattern(
        R"(^B(0|-)(1|-)(2|-)(3|-)(4|-)(5|-):R[0-5-]:W[0-5-]:(Y|-):S\d{2})");
    return std::regex_search(s, pattern);
}

CuControlCode::CuControlCode(std::uint32_t code) {
    splitCode(code, m_Barrier, m_Read, m_Write, m_Yield, m_Stall);
}

bool CuControlCode::isYield() const {
    return m_Yield == 0;
}

std::uint32_t CuControlCode::getStallCount() const {
    return m_Stall;
}

int CuControlCode::getReadSB() const {
    return m_Read == 7 ? -1 : static_cast<int>(m_Read);
}

int CuControlCode::getWriteSB() const {
    return m_Write == 7 ? -1 : static_cast<int>(m_Write);
}

std::set<int> CuControlCode::getBarrierSet() const {
    std::set<int> barriers;
    for (int i = 0; i < 6; ++i) {
        if ((m_Barrier & (1u << i)) > 0) {
            barriers.insert(i);
        }
    }
    return barriers;
}

void CuControlCode::splitCode(std::uint32_t code, std::uint32_t& waitbar, std::uint32_t& readbar,
                               std::uint32_t& writebar, std::uint32_t& yieldFlag, std::uint32_t& stall) {
    stall = (code & 0x0000fu) >> 0;
    yieldFlag = (code & 0x00010u) >> 4;
    writebar = (code & 0x000e0u) >> 5;
    readbar = (code & 0x00700u) >> 8;
    waitbar = (code & 0x1f800u) >> 11;
}

void CuControlCode::splitCode2(std::uint32_t code, std::uint32_t& waitbar, std::uint32_t& readbar,
                                std::uint32_t& writebar, std::uint32_t& ystall) {
    ystall = (code & 0x0001fu) >> 0;
    writebar = (code & 0x000e0u) >> 5;
    readbar = (code & 0x00700u) >> 8;
    waitbar = (code & 0x1f800u) >> 11;
}

std::uint32_t CuControlCode::mergeCode(std::uint32_t waitbar, std::uint32_t readbar, std::uint32_t writebar,
                                        std::uint32_t yieldFlag, std::uint32_t stall) {
    std::uint32_t code = waitbar << 11;
    code += readbar << 8;
    code += writebar << 5;
    code += yieldFlag << 4;
    code += stall;
    return code;
}

std::string CuControlCode::decode(std::uint32_t code) {
    std::uint32_t waitbar = 0;
    std::uint32_t readbar = 0;
    std::uint32_t writebar = 0;
    std::uint32_t yieldFlag = 0;
    std::uint32_t stall = 0;
    splitCode(code, waitbar, readbar, writebar, yieldFlag, stall);

    const std::string sYield = (yieldFlag != 0) ? "-" : "Y";
    const std::string sWritebar = (writebar == 7) ? "-" : std::to_string(writebar);
    const std::string sReadbar = (readbar == 7) ? "-" : std::to_string(readbar);

    std::string sWaitbar;
    for (int i = 0; i < 6; ++i) {
        sWaitbar += (waitbar & (1u << i)) == 0 ? "-" : std::to_string(i);
    }

    std::ostringstream sStall;
    sStall << std::setfill('0') << std::setw(2) << stall;

    return "B" + sWaitbar + ":R" + sReadbar + ":W" + sWritebar + ":" + sYield + ":S" + sStall.str();
}

std::uint32_t CuControlCode::encode(const std::string& s) {
    if (!matchesControlCodePattern(s)) {
        throw std::invalid_argument("Invalid control code strings: " + s + " !!!");
    }

    const std::vector<std::string> tokens = splitString(s, ':');

    std::uint32_t waitbar = 0;
    for (int i = 0; i < 6; ++i) {
        if (tokens[0][1 + i] != '-') {
            waitbar |= (1u << i);
        }
    }

    const char readbarChar = tokens[1][1];
    const std::uint32_t readbar = (readbarChar == '-') ? 7u : static_cast<std::uint32_t>(readbarChar - '0');

    const char writebarChar = tokens[2][1];
    const std::uint32_t writebar = (writebarChar == '-') ? 7u : static_cast<std::uint32_t>(writebarChar - '0');

    const std::uint32_t yieldFlag = (tokens[3] == "Y") ? 0u : 1u;
    const std::uint32_t stall = static_cast<std::uint32_t>(std::stoul(tokens[4].substr(1)));

    return mergeCode(waitbar, readbar, writebar, yieldFlag, stall);
}

} // namespace CuAsm
