#include <cstring>
#include <filesystem>
#include <string>

#include "../CuAsm/config.hpp"
#include "utils/TestUtilsCommon.hpp"

namespace fs = std::filesystem;
using CuAsm::Config;

/**
 * @brief Exercises CuAsm::Config: the hard-coded default ELF headers (parsed once from a literal
 *        byte sequence, mirroring the python original's approach), and the InsAsmRepos/IOInfo/
 *        LatencyClass path-resolution helpers, checked against the actual files shipped under
 *        CuAsm/InsAsmRepos/ (DefaultInsAsmRepos.sm_{60,61,70,75,80,86}.txt; IOInfo.sm_{75,80,86}.txt
 *        per Reports/tasks.md Phase 0; LatencyClass.sm_{75,80,86}.txt per Phase 1) -- for version
 *        numbers without a per-version file of a given kind, the corresponding getDefault*File
 *        falls back to that kind's shared "*.all.*" file instead.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    // ---- defaultCubinFileHeader: parsed from a literal hex byte sequence at static-init time ----

    const ELFIO::Elf64_Ehdr& hdr = Config::defaultCubinFileHeader();
    t.check("the default cubin header starts with the ELF magic bytes",
            hdr.e_ident[ELFIO::EI_MAG0] == ELFIO::ELFMAG0 && hdr.e_ident[ELFIO::EI_MAG1] == 'E' &&
                hdr.e_ident[ELFIO::EI_MAG2] == 'L' && hdr.e_ident[ELFIO::EI_MAG3] == 'F');
    t.check("the default cubin header is 64-bit, little-endian",
            hdr.e_ident[ELFIO::EI_CLASS] == ELFIO::ELFCLASS64 && hdr.e_ident[ELFIO::EI_DATA] == ELFIO::ELFDATA2LSB);
    t.checkEqual("the default cubin header's machine type is EM_CUDA (190), matching the constant "
                 "CuAsmParser's own .cuasm '.__elf_machine EM_CUDA' directive resolves to",
                 hdr.e_machine, static_cast<ELFIO::Elf_Half>(190));
    t.check("repeated calls to defaultCubinFileHeader() return the exact same static instance",
            &Config::defaultCubinFileHeader() == &hdr);

    // ---- the other default headers/entries are all zero-initialized ----

    ELFIO::Elf64_Shdr zeroShdr{};
    ELFIO::Elf64_Phdr zeroPhdr{};
    ELFIO::Elf64_Sym zeroSym{};
    ELFIO::Elf64_Rel zeroRel{};
    ELFIO::Elf64_Rela zeroRela{};
    t.check("defaultSectionHeader/defaultSegmentHeader/defaultSymbol/defaultRel/defaultRela are all zero-initialized",
            std::memcmp(&Config::defaultSectionHeader(), &zeroShdr, sizeof(zeroShdr)) == 0 &&
                std::memcmp(&Config::defaultSegmentHeader(), &zeroPhdr, sizeof(zeroPhdr)) == 0 &&
                std::memcmp(&Config::defaultSymbol(), &zeroSym, sizeof(zeroSym)) == 0 &&
                std::memcmp(&Config::defaultRel(), &zeroRel, sizeof(zeroRel)) == 0 &&
                std::memcmp(&Config::defaultRela(), &zeroRela, sizeof(zeroRela)) == 0);

    // ---- getDefaultInsAsmReposFile: points at a real, shipped file for a supported version ----

    const std::string reposPath = Config::getDefaultInsAsmReposFile(61);
    t.check("getDefaultInsAsmReposFile(61) builds the expected filename and points at a file that "
            "actually exists (DefaultInsAsmRepos.sm_61.txt ships in CuAsm/InsAsmRepos/)",
            reposPath.ends_with("DefaultInsAsmRepos.sm_61.txt") && fs::is_regular_file(reposPath));

    t.check("getDefaultInsAsmReposFile builds a distinct, well-formed path per version number, "
            "regardless of whether that version's file happens to exist",
            Config::getDefaultInsAsmReposFile(89).ends_with("DefaultInsAsmRepos.sm_89.txt") &&
                !fs::is_regular_file(Config::getDefaultInsAsmReposFile(89)));

    // ---- getDefaultIOInfoFile: no IOInfo.sm_*.txt file ships for version 61 or 999, so both
    //      fall back to the shared IOInfo.all.json ----

    t.check("getDefaultIOInfoFile falls back to IOInfo.all.json when no per-version file exists "
            "(true for 61 and 999, which have no IOInfo.sm_*.txt of their own)",
            Config::getDefaultIOInfoFile(61).ends_with("IOInfo.all.json") &&
                Config::getDefaultIOInfoFile(999).ends_with("IOInfo.all.json"));

    // ---- getDefaultLatencyClassFile: mirrors getDefaultIOInfoFile's fallback behavior, checked
    //      against the real shipped LatencyClass.sm_75.txt (Reports/tasks.md Phase 1) ----

    const std::string latencyPath = Config::getDefaultLatencyClassFile(75);
    t.check("getDefaultLatencyClassFile(75) builds the expected filename and points at a file "
            "that actually exists (LatencyClass.sm_75.txt ships in CuAsm/InsAsmRepos/)",
            latencyPath.ends_with("LatencyClass.sm_75.txt") && fs::is_regular_file(latencyPath));

    t.check("getDefaultLatencyClassFile falls back to LatencyClass.all.txt when no per-version "
            "file exists (true for 61 and 999, which have no LatencyClass.sm_*.txt of their own)",
            Config::getDefaultLatencyClassFile(61).ends_with("LatencyClass.all.txt") &&
                Config::getDefaultLatencyClassFile(999).ends_with("LatencyClass.all.txt"));

    return t.finish("test_config");
}
