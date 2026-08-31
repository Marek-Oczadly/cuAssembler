#include "OperandRole.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "CuAsmLogger.hpp"
#include "CuSMVersion.hpp"
#include "common.hpp"
#include "config.hpp"

namespace CuAsm {

namespace {

namespace fs = std::filesystem;

const std::array<std::pair<AccessMode, const char*>, 3>& accessModeTable() {
    static const std::array<std::pair<AccessMode, const char*>, 3> t{{
        {AccessMode::READ, "READ"},
        {AccessMode::WRITE, "WRITE"},
        {AccessMode::READ_WRITE, "READ_WRITE"},
    }};
    return t;
}

const std::array<std::pair<OperandKind, const char*>, 16>& operandKindTable() {
    static const std::array<std::pair<OperandKind, const char*>, 16> t{{
        {OperandKind::GPR, "R"},
        {OperandKind::UGPR, "UR"},
        {OperandKind::PRED, "P"},
        {OperandKind::UPRED, "UP"},
        {OperandKind::BARRIER, "B"},
        {OperandKind::SBREG, "SB"},
        {OperandKind::SBSET, "SBSET"},
        {OperandKind::SREG, "SR"},
        {OperandKind::INT_IMME, "II"},
        {OperandKind::FLOAT_IMME, "FI"},
        {OperandKind::R_ADDR, "R.Addr"},
        {OperandKind::UR_ADDR, "UR.Addr"},
        {OperandKind::IMME_ADDR, "Imme.Addr"},
        {OperandKind::CBANK_IMME, "Imme.CBank"},
        {OperandKind::UR_CBANK, "UR.CBank"},
        {OperandKind::UR_DESC, "UR.Desc"},
    }};
    return t;
}

/** @brief Splits on runs of ASCII whitespace, discarding empty tokens, unlike common.hpp's splitChar. */
std::vector<std::string> splitWhitespace(const std::string& s) {
    std::istringstream iss(s);
    std::vector<std::string> tokens;
    std::string tok;
    while (iss >> tok) {
        tokens.push_back(tok);
    }
    return tokens;
}

/** @brief Parses one "kind:mode kind:mode ..." role-list, shared by InsKey lines and override lines. */
std::vector<OperandRoleEntry> parseRoleList(const std::string& text, const std::string& context) {
    std::vector<OperandRoleEntry> roles;
    for (const std::string& tok : splitWhitespace(text)) {
        const std::size_t colon = tok.find(':');
        if (colon == std::string::npos) {
            throw std::runtime_error("IOInfo parse error: malformed role token \"" + tok + "\" for \"" + context + "\"");
        }
        OperandRoleEntry entry;
        try {
            entry.kind = parseOperandKind(tok.substr(0, colon));
            entry.mode = parseAccessMode(tok.substr(colon + 1));
        } catch (const std::invalid_argument& e) {
            throw std::runtime_error("IOInfo parse error: " + std::string(e.what()) + " for \"" + context + "\"");
        }
        roles.push_back(entry);
    }
    return roles;
}

} // namespace

std::string toString(AccessMode mode) {
    for (const auto& [m, s] : accessModeTable()) {
        if (m == mode) {
            return s;
        }
    }
    throw std::invalid_argument("toString(AccessMode): unknown enumerator");
}

AccessMode parseAccessMode(const std::string& s) {
    for (const auto& [m, name] : accessModeTable()) {
        if (s == name) {
            return m;
        }
    }
    throw std::invalid_argument("parseAccessMode: unknown token \"" + s + "\"");
}

std::string toString(OperandKind kind) {
    for (const auto& [k, s] : operandKindTable()) {
        if (k == kind) {
            return s;
        }
    }
    throw std::invalid_argument("toString(OperandKind): unknown enumerator");
}

OperandKind parseOperandKind(const std::string& tag) {
    for (const auto& [k, name] : operandKindTable()) {
        if (tag == name) {
            return k;
        }
    }
    throw std::invalid_argument("parseOperandKind: unknown tag \"" + tag + "\"");
}

void OperandRoleTable::initFromFile(const std::string& fileName) {
    std::ifstream fin(fileName);
    if (!fin) {
        throw std::runtime_error("Cannot open IOInfo file " + fileName + "!");
    }
    std::ostringstream buf;
    buf << fin.rdbuf();
    parse(buf.str());
}

void OperandRoleTable::parse(const std::string& text) {
    std::istringstream lines(text);
    std::string rawLine;
    std::string currentKey;

    while (std::getline(lines, rawLine)) {
        if (!rawLine.empty() && rawLine.back() == '\r') {
            rawLine.pop_back();
        }
        const bool indented = !rawLine.empty() && (rawLine.front() == ' ' || rawLine.front() == '\t');

        const std::size_t hashPos = rawLine.find('#');
        std::string content = trim(hashPos == std::string::npos ? rawLine : rawLine.substr(0, hashPos));
        if (content.empty()) {
            continue;
        }

        if (indented) {
            if (currentKey.empty()) {
                throw std::runtime_error("IOInfo parse error: override line before any InsKey line: \"" + content + "\"");
            }
            if (content.front() != '@') {
                throw std::runtime_error("IOInfo parse error: override line must start with '@': \"" + content + "\"");
            }
            const std::size_t colon = content.find(':');
            if (colon == std::string::npos) {
                throw std::runtime_error("IOInfo parse error: override line missing ':': \"" + content + "\"");
            }
            OperandRoleOverride ov;
            ov.modifier = trim(content.substr(1, colon - 1));
            ov.roles = parseRoleList(content.substr(colon + 1), currentKey + " override \"" + ov.modifier + "\"");
            m_Table.at(currentKey).overrides.push_back(std::move(ov));
        } else {
            const std::size_t colon = content.find(':');
            if (colon == std::string::npos) {
                throw std::runtime_error("IOInfo parse error: InsKey line missing ':': \"" + content + "\"");
            }
            const std::string insKey = trim(content.substr(0, colon));
            OperandRoleRecord record;
            record.roles = parseRoleList(content.substr(colon + 1), insKey);
            m_Table.insert_or_assign(insKey, std::move(record));
            currentKey = insKey;
        }
    }
}

const std::vector<OperandRoleEntry>& OperandRoleTable::lookup(const std::string& insKey, const std::vector<std::string>& insModi) const {
    const auto it = m_Table.find(insKey);
    if (it == m_Table.end()) {
        throw std::out_of_range("OperandRoleTable::lookup: no curated entry for InsKey \"" + insKey + "\"");
    }
    const OperandRoleRecord& record = it->second;
    for (const OperandRoleOverride& ov : record.overrides) {
        if (std::find(insModi.begin(), insModi.end(), ov.modifier) != insModi.end()) {
            return ov.roles;
        }
    }
    return record.roles;
}

bool OperandRoleTable::contains(const std::string& insKey) const {
    return m_Table.find(insKey) != m_Table.end();
}

std::size_t OperandRoleTable::size() const {
    return m_Table.size();
}

OperandRoleTable OperandRoleTable::getDefaultTable(int versionNumber) {
    OperandRoleTable table;

    const std::string fname = Config::getDefaultIOInfoFile(versionNumber);
    if (fs::exists(fname)) {
        table.initFromFile(fname);
        return table;
    }

    const auto aliasIt = CuSMVersion::InsAsmReposAliasDict.find(versionNumber);
    if (aliasIt != CuSMVersion::InsAsmReposAliasDict.end()) {
        const std::string aname = Config::getDefaultIOInfoFile(aliasIt->second);
        if (fs::exists(aname)) {
            CuAsmLogger::logWarning("No IOInfo table for SM_" + std::to_string(versionNumber) + " found! Using SM_" +
                                     std::to_string(aliasIt->second) + " instead...");
            table.initFromFile(aname);
            return table;
        }
    }

    CuAsmLogger::logError("No IOInfo table (or alias) for SM_" + std::to_string(versionNumber) +
                           " found! Using an empty table -- every lookup() will throw until one is curated.");
    return table;
}

OperandRoleTable& OperandRoleTable::getStaticTable(int versionNumber) {
    static std::map<int, OperandRoleTable> s_staticTables;
    auto it = s_staticTables.find(versionNumber);
    if (it == s_staticTables.end()) {
        it = s_staticTables.emplace(versionNumber, getDefaultTable(versionNumber)).first;
    }
    return it->second;
}

} // namespace CuAsm
