#include <cstdint>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include <elfio/elfio.hpp>

#include "../CuAsm/common.hpp"
#include "utils/TestUtilsCommon.hpp"

namespace fs = std::filesystem;

namespace {

using OffsetSymbolSet = std::set<std::pair<ELFIO::Elf64_Addr, std::string>>;

/**
 * @brief Extracts every (offset, symbol) pair cuobjdump's own text dump reports for each REL
 *        section of a cubin, by running `cuobjdump -elf` and parsing lines of the form
 *        "0x<offset>    <symbol>    <TYPE_NAME>" under each ".section <name>	REL" header.
 * @param binname Cubin to dump.
 * @return Map from relocation section name to its (offset, symbol) pairs, per cuobjdump.
 **/
std::map<std::string, OffsetSymbolSet> getRelSectionInfoFromCuobjdump(const std::string& binname) {
    static const std::regex sectionHeaderRe(R"(^\.section\s+(\S+)\s+REL)");
    // A RELA section's lines carry a 4th (addend) column REL's don't; the addend isn't needed for
    // this comparison (offset + symbol only), so it's just optionally consumed here.
    static const std::regex relLineRe(R"(^0x([0-9a-fA-F]+)\s+(\S+)\s+(\S+)(?:\s+\S+)?$)");

    const std::string dump = CuAsm::checkOutput({"cuobjdump", "-elf", binname});

    std::map<std::string, OffsetSymbolSet> result;
    std::string currentSection;
    bool inSection = false;

    std::istringstream iss(dump);
    std::string rawLine;
    while (std::getline(iss, rawLine)) {
        const std::string line = CuAsm::trim(rawLine);

        std::smatch m;
        if (std::regex_search(line, m, sectionHeaderRe)) {
            currentSection = m[1].str();
            inSection = true;
            continue;
        }
        if (line.empty()) {
            inSection = false;
            continue;
        }
        if (inSection && std::regex_match(line, m, relLineRe)) {
            const auto offset = static_cast<ELFIO::Elf64_Addr>(std::stoull(m[1].str(), nullptr, 16));
            result[currentSection].emplace(offset, m[2].str());
        }
    }

    return result;
}

/**
 * @brief Extracts every (offset, symbol) pair straight from each ".rel*" section's ELF relocation
 *        entries, resolving each entry's symbol index against .symtab.
 * @param binname Cubin to read.
 * @return Map from relocation section name to its (offset, symbol) pairs, per ELFIO.
 **/
std::map<std::string, OffsetSymbolSet> getRelSectionInfoFromElf(const std::string& binname) {
    std::map<std::string, OffsetSymbolSet> result;

    ELFIO::elfio ef;
    if (!ef.load(binname)) {
        return result;
    }

    ELFIO::section* symtab = ef.sections[".symtab"];
    for (const auto& sec : ef.sections) {
        if (!sec->get_name().starts_with(".rel")) {
            continue;
        }

        const ELFIO::relocation_section_accessor rela(ef, sec.get());
        OffsetSymbolSet entries;
        const ELFIO::Elf_Xword n = rela.get_entries_num();
        for (ELFIO::Elf_Xword i = 0; i < n; ++i) {
            ELFIO::Elf64_Addr offset = 0;
            ELFIO::Elf_Word symbol = 0;
            ELFIO::Elf_Word type = 0;
            ELFIO::Elf_Sxword addend = 0;
            rela.get_entry(i, offset, symbol, type, addend);

            std::string symName;
            ELFIO::Elf64_Addr symValue = 0;
            ELFIO::Elf_Xword symSize = 0;
            unsigned char symBind = 0, symType = 0, symOther = 0;
            ELFIO::Elf_Half symSection = 0;
            const ELFIO::symbol_section_accessor symAccessor(ef, symtab);
            symAccessor.get_symbol(symbol, symName, symValue, symSize, symBind, symType, symSection, symOther);

            entries.emplace(offset, symName);
        }
        result[sec->get_name()] = std::move(entries);
    }

    return result;
}

} // namespace

/**
 * @brief Cross-validates CuAsm's own ELF relocation parsing (the same relocation_section_accessor
 *        pattern CuAsmParser/CubinFile use internally) against NVIDIA's own cuobjdump for every
 *        REL section of the CuTest fixture cubins. Previously this file only printed both sources
 *        side by side with no comparison at all - a real discrepancy (wrong offset, wrong symbol
 *        resolution) would have been visible only to a human reading the output by eye, if at all.
 *        cuobjdump is confirmed to run standalone in this environment (unlike nvcc, which needs an
 *        MSVC host-compiler environment this test harness doesn't set up).
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    const std::string dir = std::string(CUASM_TESTDATA_DIR) + "/CuTest";
    int cubinsChecked = 0;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".cubin") {
            continue;
        }
        ++cubinsChecked;
        const std::string fname = entry.path().string();

        const std::map<std::string, OffsetSymbolSet> fromElf = getRelSectionInfoFromElf(fname);
        const std::map<std::string, OffsetSymbolSet> fromDump = getRelSectionInfoFromCuobjdump(fname);

        t.check(fname + ": at least one REL section was found by both ELFIO and cuobjdump",
                !fromElf.empty() && !fromDump.empty());

        bool everySectionMatches = true;
        for (const auto& [secName, elfEntries] : fromElf) {
            const auto it = fromDump.find(secName);
            if (it == fromDump.end() || it->second != elfEntries) {
                everySectionMatches = false;
            }
        }
        t.check(fname + ": every REL section's (offset, symbol) pairs, parsed independently via "
                "ELFIO, exactly match cuobjdump's own dump of the same section",
                everySectionMatches);
    }

    t.check("at least one .cubin fixture was actually found and checked under " + dir, cubinsChecked > 0);

    return t.finish("test_relocation");
}
