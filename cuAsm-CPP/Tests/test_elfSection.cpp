#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <elfio/elfio.hpp>

/**
 * @brief Builds a dict mapping each null-terminated byte string found in bytelist to its
 *        (start, end) byte offsets, mirroring the original buildStringDict().
 * @param bytelist Buffer to scan for null-terminated strings.
 * @return Map from each distinct string (including its trailing '\0') to its first (start, end) span.
 **/
std::map<std::string, std::pair<size_t, size_t>> buildStringDict(const std::string& bytelist) {
    std::map<std::string, std::pair<size_t, size_t>> sdict;
    size_t p = 0;

    while (true) {
        size_t pnext = bytelist.find('\0', p);
        if (pnext == std::string::npos) {
            break;
        }

        std::string s = bytelist.substr(p, pnext + 1 - p);
        if (sdict.find(s) == sdict.end()) {
            sdict[s] = {p, pnext + 1};
        }
        p = pnext + 1;
    }

    return sdict;
}

/**
 * @brief Prints every null-terminated byte string in bs as a labeled ".byte" hex dump, mirroring
 *        the original bytes2text().
 * @param bs Buffer to scan for null-terminated strings.
 * @param label Label prefixed to each printed string's index.
 **/
void bytes2text(const std::string& bs, const std::string& label = "") {
    size_t p = 0;
    int counter = 0;

    while (true) {
        counter += 1;
        size_t pnext = bs.find('\0', p);
        if (pnext == std::string::npos) {
            break;
        }

        std::string s = bs.substr(p, pnext + 1 - p);
        std::printf("    // %4zu: %s[%d] = %s \n", p, label.c_str(), counter, s.c_str());

        size_t p0 = p;
        while (p0 < pnext + 1) {
            std::printf("    /*%04zx*/ .byte ", p0);
            for (size_t i = p0 - p; i < p0 - p + 8 && i < s.size(); ++i) {
                if (i > (p0 - p)) std::printf(", ");
                std::printf("0x%02x", static_cast<unsigned char>(s[i]));
            }
            std::printf("\n");
            p0 += 8;
        }

        std::printf("\n");
        p = pnext + 1;
    }
}

/**
 * @brief Entry point mirroring the original Tests/test_elfSection.py script: loads a cubin's ELF
 *        structures via ELFIO and prints its header, sections, and segments.
 * @return 0 on success.
 **/
int main() {
    std::string binname = R"(G:\Repos\Tests\Programs\cudatest.7.sm_75.cubin)";

    ELFIO::elfio ef;
    if (!ef.load(binname)) {
        std::cerr << "Failed to load " << binname << std::endl;
        return 1;
    }

    // s1 = ef.get_section_by_name('.strtab')
    // s2 = ef.get_section_by_name('.shstrtab')
    // s3 = ef.get_section_by_name('.symtab')
    // s4 = ef.get_section_by_name('.nv.info')
    ELFIO::section* s1 = ef.sections[".strtab"];
    ELFIO::section* s2 = ef.sections[".shstrtab"];
    ELFIO::section* s3 = ef.sections[".symtab"];
    ELFIO::section* s4 = ef.sections[".nv.info"];
    (void)s1;
    (void)s2;
    (void)s3;
    (void)s4;

    std::cout << "e_type      = " << ef.get_type() << std::endl;
    std::cout << "e_machine   = " << ef.get_machine() << std::endl;
    std::cout << "e_version   = " << ef.get_version() << std::endl;
    std::cout << "e_entry     = " << ef.get_entry() << std::endl;
    std::cout << "e_phoff     = " << ef.get_segments_offset() << std::endl;
    std::cout << "e_shoff     = " << ef.get_sections_offset() << std::endl;
    std::cout << "e_flags     = 0x" << std::hex << ef.get_flags() << std::dec << std::endl;
    std::cout << "e_ehsize    = " << ef.get_header_size() << std::endl;
    std::cout << "e_phentsize = " << ef.get_segment_entry_size() << std::endl;
    std::cout << "e_phnum     = " << ef.segments.size() << std::endl;
    std::cout << "e_shentsize = " << ef.get_section_entry_size() << std::endl;
    std::cout << "e_shnum     = " << ef.sections.size() << std::endl;
    std::cout << "e_shstrndx  = " << ef.get_section_name_str_index() << std::endl;
    std::cout << std::endl;

    for (const auto& sec : ef.sections) {
        ELFIO::Elf64_Addr s0 = sec->get_offset();
        ELFIO::Elf64_Addr s1_ = s0 + sec->get_size();
        std::printf("%-32s  %8llu   %8llu   %8llu   %8llu\n",
                    sec->get_name().c_str(),
                    static_cast<unsigned long long>(s0),
                    static_cast<unsigned long long>(sec->get_size()),
                    static_cast<unsigned long long>(s1_),
                    static_cast<unsigned long long>(sec->get_addr_align()));
    }

    std::cout << std::endl;
    for (const auto& seg : ef.segments) {
        std::cout << "p_type   = " << seg->get_type() << std::endl;
        std::cout << "p_flags  = " << seg->get_flags() << std::endl;
        std::cout << "p_offset = " << seg->get_offset() << std::endl;
        std::cout << "p_vaddr  = " << seg->get_virtual_address() << std::endl;
        std::cout << "p_paddr  = " << seg->get_physical_address() << std::endl;
        std::cout << "p_filesz = " << seg->get_file_size() << std::endl;
        std::cout << "p_memsz  = " << seg->get_memory_size() << std::endl;
        std::cout << "p_align  = " << seg->get_align() << std::endl;
        std::cout << std::endl;
    }

    return 0;
}
