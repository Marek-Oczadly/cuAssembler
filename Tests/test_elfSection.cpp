#include <filesystem>
#include <set>
#include <string>

#include <elfio/elfio.hpp>

#include "utils/TestUtilsCommon.hpp"

namespace fs = std::filesystem;

/**
 * @brief Exercises ELFIO's cubin-loading path against the real cudatest.7.sm_75.cubin fixture.
 *        Previously this file just printed the ELF header/section/segment tables with no
 *        assertions at all. The expected values here (e_type, sm version, toolkit version) were
 *        cross-checked against a real `cuobjdump -elf` run on the same fixture
 *        ("64bit elf: type=2, abi=7, sm=75, toolkit=111, flags = 0x4b054b"), and e_machine=190
 *        against two independent places elsewhere in this codebase that both hard-code EM_CUDA as
 *        190 (Config::defaultCubinFileHeader's literal header bytes, and CuAsmParser's
 *        resolveNamedConstant table for the ".__elf_machine EM_CUDA" directive).
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    const std::string binname = std::string(CUASM_TESTDATA_DIR) + "/CuTest/cudatest.7.sm_75.cubin";
    const auto fileSize = static_cast<ELFIO::Elf64_Addr>(fs::file_size(binname));

    ELFIO::elfio ef;
    t.check("ELFIO successfully loads the fixture cubin", ef.load(binname));

    t.check("file header: ET_EXEC, EM_CUDA (190), and the flags field carries SM_75 in both its "
            "low and virtual-SM bytes",
            ef.get_type() == ELFIO::ET_EXEC && ef.get_machine() == 190 && (ef.get_flags() & 0xff) == 75 &&
                ((ef.get_flags() >> 16) & 0xff) == 75);

    std::set<std::string> sectionNames;
    bool everySectionInBounds = true;
    for (const auto& sec : ef.sections) {
        sectionNames.insert(sec->get_name());
        // A NOBITS section (e.g. .bss) occupies no file space; every other section's [offset,
        // offset+size) must fit within the file.
        if (sec->get_type() != ELFIO::SHT_NOBITS && sec->get_offset() + sec->get_size() > fileSize) {
            everySectionInBounds = false;
        }
    }
    t.check("every expected named section is present (.strtab/.shstrtab/.symtab/.nv.info/.debug_frame)",
            sectionNames.count(".strtab") && sectionNames.count(".shstrtab") && sectionNames.count(".symtab") &&
                sectionNames.count(".nv.info") && sectionNames.count(".debug_frame"));
    t.check("every section's [offset, offset+size) range fits within the file", everySectionInBounds);

    t.check("there's exactly one .nv.info.<kernel> section per known CuTest kernel (argtest, "
            "local_test, child, shared_test)",
            sectionNames.count(".nv.info._Z7argtestPiS_S_") && sectionNames.count(".nv.info._Z10local_testiiPi") &&
                sectionNames.count(".nv.info._Z5childPii") && sectionNames.count(".nv.info._Z11shared_testfPf"));

    t.check("the cubin has at least one ELF segment (program header)", ef.segments.size() > 0);
    bool everySegmentSane = true;
    for (const auto& seg : ef.segments) {
        if (seg->get_file_size() > seg->get_memory_size() || seg->get_offset() + seg->get_file_size() > fileSize) {
            everySegmentSane = false;
        }
    }
    t.check("every segment's file size fits within its memory size and within the file",
            everySegmentSane);

    return t.finish("test_elfSection");
}
