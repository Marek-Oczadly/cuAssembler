#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <elfio/elfio.hpp>

#include "../CuAsm/common.hpp"

namespace fs = std::filesystem;

/// One relocation entry: (symbol name, offset, relocation type, 8 raw bytes read from the section
/// being relocated), mirroring the (symname, rel, val) tuples of the original getELFRelInfo().
struct RelEntry {
    std::string symName;
    ELFIO::Elf64_Addr offset;
    ELFIO::Elf_Word type;
    std::vector<std::uint8_t> val;
};

/**
 * @brief Runs `cuobjdump -elf` on a cubin and extracts, for each "REL" section, the raw text lines
 *        cuobjdump prints for it, mirroring the original getRelSectionInfo().
 * @param binname Path of the cubin to dump.
 * @return Map from section name to its list of raw dumped-relocation text lines.
 **/
std::map<std::string, std::vector<std::string>> getRelSectionInfo(const std::string& binname) {
    static const std::regex sectionPattern(R"(\.section\s+([.\w]+)\s*REL)");

    std::string dump = CuAsm::checkOutput({"cuobjdump", "-elf", binname});

    std::map<std::string, std::vector<std::string>> relSecDict;
    std::string sname;
    std::vector<std::string> rellines;
    bool doGather = false;

    std::istringstream iss(dump);
    std::string rawLine;
    while (std::getline(iss, rawLine)) {
        std::string line = rawLine;
        // strip leading/trailing whitespace, mirroring python's str.strip()
        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end = line.find_last_not_of(" \t\r\n");
        line = (start == std::string::npos) ? "" : line.substr(start, end - start + 1);

        if (doGather) {
            if (line.empty()) {
                relSecDict[sname] = rellines;
                doGather = false;
            } else {
                rellines.push_back(line);
            }
            continue;
        }

        std::smatch m;
        if (std::regex_search(line, m, sectionPattern)) {
            // std::regex_search, not an anchored match, but cuobjdump's section lines always
            // start with ".section", so this mirrors python's re.match() here.
            if (line.rfind(".section", 0) == 0) {
                sname = m[1].str();
                rellines.clear();
                doGather = true;
            }
        }
    }

    if (doGather) {
        if (relSecDict.find(sname) == relSecDict.end()) {
            relSecDict[sname] = rellines;
        }
    }

    return relSecDict;
}

/**
 * @brief Reads every ".rel*" section's relocations straight from the ELF structures, mirroring the
 *        original getELFRelInfo().
 * @param binname Path of the cubin to read.
 * @return Map from relocation section name to its list of relocation entries.
 **/
std::map<std::string, std::vector<RelEntry>> getELFRelInfo(const std::string& binname) {
    std::map<std::string, std::vector<RelEntry>> relSecDict;

    ELFIO::elfio ef;
    if (!ef.load(binname)) {
        std::cerr << "Failed to load " << binname << std::endl;
        return relSecDict;
    }

    static const std::regex relPrefix(R"(^\.rela?)");

    for (const auto& sec : ef.sections) {
        if (sec->get_name().rfind(".rel", 0) != 0) {
            continue;
        }

        ELFIO::relocation_section_accessor rela(ef, sec.get());
        std::vector<RelEntry> rels;

        std::string mname = std::regex_replace(sec->get_name(), relPrefix, "");
        ELFIO::section* secmain = ef.sections[mname];
        if (secmain == nullptr) {
            std::cout << "Unmatched section " << sec->get_name() << std::endl;
            continue;
        }

        ELFIO::Elf_Xword n = rela.get_entries_num();
        for (ELFIO::Elf_Xword i = 0; i < n; ++i) {
            ELFIO::Elf64_Addr offset = 0;
            ELFIO::Elf_Word symbol = 0;
            ELFIO::Elf_Word type = 0;
            ELFIO::Elf_Sxword addend = 0;
            std::string symName;
            ELFIO::Elf64_Addr symValue = 0;
            unsigned char symBind = 0;
            unsigned char symType = 0;
            ELFIO::Elf_Half symSection = 0;
            unsigned char symOther = 0;

            rela.get_entry(i, offset, symbol, type, addend);

            const ELFIO::symbol_section_accessor symAccessor(ef, ef.sections[".symtab"]);
            ELFIO::Elf_Xword symSize = 0;
            symAccessor.get_symbol(symbol, symName, symValue, symSize, symBind, symType, symSection, symOther);

            const char* mainData = secmain->get_data();
            std::vector<std::uint8_t> val;
            if (mainData != nullptr && offset + 8 <= secmain->get_size()) {
                val.assign(mainData + offset, mainData + offset + 8);
            }

            rels.push_back(RelEntry{symName, offset, type, val});
        }

        relSecDict[sec->get_name()] = rels;
    }

    return relSecDict;
}

/**
 * @brief Formats a byte vector as a lowercase hex string, mirroring python bytes.hex().
 * @param val Bytes to format.
 * @return The hex-encoded string.
 **/
std::string toHex(const std::vector<std::uint8_t>& val) {
    std::string s;
    s.reserve(val.size() * 2);
    char buf[3];
    for (auto b : val) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        s += buf;
    }
    return s;
}

/**
 * @brief Entry point mirroring the original Tests/test_relocation.py script: cross-checks ELF
 *        relocation entries against cuobjdump's own dumped relocation text for every cubin in a
 *        directory.
 * @return 0 on success.
 **/
int main() {
    std::string fdir = std::string(CUASM_TESTDATA_DIR) + "/CuTest";

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fdir, ec)) {
        if (entry.path().extension() != ".cubin") {
            continue;
        }

        std::string fname = entry.path().string();
        std::cout << "#### " << fname << std::endl;

        auto d1 = getELFRelInfo(fname);
        auto d2 = getRelSectionInfo(fname);

        for (const auto& [secName, entries1] : d1) {
            std::cout << "Section " << secName << " :" << std::endl;

            auto it2 = d2.find(secName);
            const std::vector<std::string>& entries2 = (it2 != d2.end()) ? it2->second : std::vector<std::string>{};

            size_t n = std::min(entries1.size(), entries2.size());
            for (size_t i = 0; i < n; ++i) {
                const RelEntry& r1 = entries1[i];
                std::printf("  %-6llu %s    type=%2u, val=%s\n",
                            static_cast<unsigned long long>(r1.offset), r1.symName.c_str(),
                            static_cast<unsigned>(r1.type), toHex(r1.val).c_str());
                std::cout << "  " << entries2[i] << std::endl;
                std::cout << std::endl;
            }
        }
    }

    return 0;
}
