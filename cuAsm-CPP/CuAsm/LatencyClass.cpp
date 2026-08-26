#include "LatencyClass.hpp"

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

const std::array<std::pair<LatencyKind, const char*>, 2>& latencyKindTable() {
    static const std::array<std::pair<LatencyKind, const char*>, 2> t{{
        {LatencyKind::FIXED, "FIXED"},
        {LatencyKind::VARIABLE, "VARIABLE"},
    }};
    return t;
}

const std::array<std::pair<BarrierType, const char*>, 2>& barrierTypeTable() {
    static const std::array<std::pair<BarrierType, const char*>, 2> t{{
        {BarrierType::READ, "READ"},
        {BarrierType::WRITE, "WRITE"},
    }};
    return t;
}

} // namespace

std::string toString(LatencyKind kind) {
    for (const auto& [k, s] : latencyKindTable()) {
        if (k == kind) {
            return s;
        }
    }
    throw std::invalid_argument("toString(LatencyKind): unknown enumerator");
}

LatencyKind parseLatencyKind(const std::string& s) {
    for (const auto& [k, name] : latencyKindTable()) {
        if (s == name) {
            return k;
        }
    }
    throw std::invalid_argument("parseLatencyKind: unknown token \"" + s + "\"");
}

std::string toString(BarrierType type) {
    for (const auto& [b, s] : barrierTypeTable()) {
        if (b == type) {
            return s;
        }
    }
    throw std::invalid_argument("toString(BarrierType): unknown enumerator");
}

BarrierType parseBarrierType(const std::string& s) {
    for (const auto& [b, name] : barrierTypeTable()) {
        if (s == name) {
            return b;
        }
    }
    throw std::invalid_argument("parseBarrierType: unknown token \"" + s + "\"");
}

void LatencyClassTable::initFromFile(const std::string& fileName) {
    std::ifstream fin(fileName);
    if (!fin) {
        throw std::runtime_error("Cannot open LatencyClass file " + fileName + "!");
    }
    std::ostringstream buf;
    buf << fin.rdbuf();
    parse(buf.str());
}

void LatencyClassTable::parse(const std::string& text) {
    std::istringstream lines(text);
    std::string rawLine;

    while (std::getline(lines, rawLine)) {
        if (!rawLine.empty() && rawLine.back() == '\r') {
            rawLine.pop_back();
        }

        const std::size_t hashPos = rawLine.find('#');
        const std::string content = trim(hashPos == std::string::npos ? rawLine : rawLine.substr(0, hashPos));
        if (content.empty()) {
            continue;
        }

        const std::size_t colon = content.find(':');
        if (colon == std::string::npos) {
            throw std::runtime_error("LatencyClass parse error: InsKey line missing ':': \"" + content + "\"");
        }
        const std::string insKey = trim(content.substr(0, colon));
        const std::string token = trim(content.substr(colon + 1));

        LatencyClassEntry entry;
        if (token == "FIXED") {
            entry.kind = LatencyKind::FIXED;
            entry.barrier = std::nullopt;
        } else if (token.starts_with("VARIABLE:")) {
            entry.kind = LatencyKind::VARIABLE;
            try {
                entry.barrier = parseBarrierType(token.substr(std::string("VARIABLE:").size()));
            } catch (const std::invalid_argument& e) {
                throw std::runtime_error("LatencyClass parse error: " + std::string(e.what()) + " for \"" + insKey + "\"");
            }
        } else {
            throw std::runtime_error("LatencyClass parse error: malformed classification token \"" + token + "\" for \"" + insKey +
                                      "\" (expected \"FIXED\", \"VARIABLE:READ\", or \"VARIABLE:WRITE\")");
        }

        m_Table.insert_or_assign(insKey, entry);
    }
}

const LatencyClassEntry& LatencyClassTable::lookup(const std::string& insKey) const {
    const auto it = m_Table.find(insKey);
    if (it == m_Table.end()) {
        throw std::out_of_range("LatencyClassTable::lookup: no curated entry for InsKey \"" + insKey + "\"");
    }
    return it->second;
}

bool LatencyClassTable::contains(const std::string& insKey) const {
    return m_Table.find(insKey) != m_Table.end();
}

std::size_t LatencyClassTable::size() const {
    return m_Table.size();
}

LatencyClassTable LatencyClassTable::getDefaultTable(int versionNumber) {
    LatencyClassTable table;

    const std::string fname = Config::getDefaultLatencyClassFile(versionNumber);
    if (fs::exists(fname)) {
        table.initFromFile(fname);
        return table;
    }

    const auto aliasIt = CuSMVersion::InsAsmReposAliasDict.find(versionNumber);
    if (aliasIt != CuSMVersion::InsAsmReposAliasDict.end()) {
        const std::string aname = Config::getDefaultLatencyClassFile(aliasIt->second);
        if (fs::exists(aname)) {
            CuAsmLogger::logWarning("No LatencyClass table for SM_" + std::to_string(versionNumber) + " found! Using SM_" +
                                     std::to_string(aliasIt->second) + " instead...");
            table.initFromFile(aname);
            return table;
        }
    }

    CuAsmLogger::logError("No LatencyClass table (or alias) for SM_" + std::to_string(versionNumber) +
                           " found! Using an empty table -- every lookup() will throw until one is curated.");
    return table;
}

LatencyClassTable& LatencyClassTable::getStaticTable(int versionNumber) {
    static std::map<int, LatencyClassTable> s_staticTables;
    auto it = s_staticTables.find(versionNumber);
    if (it == s_staticTables.end()) {
        it = s_staticTables.emplace(versionNumber, getDefaultTable(versionNumber)).first;
    }
    return it->second;
}

} // namespace CuAsm
