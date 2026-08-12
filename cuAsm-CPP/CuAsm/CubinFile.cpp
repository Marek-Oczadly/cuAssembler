#include "CubinFile.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <regex>
#include <span>
#include <sstream>
#include <stdexcept>

#include <elfio/elfio.hpp>

#include "CuAsmLogger.hpp"
#include "CuControlCode.hpp"
#include "CuNVInfo.hpp"
#include "common.hpp"
#include "config.hpp"
#include "utils/CubinUtils.hpp"

namespace CuAsm {

namespace {

/** @brief Maps an ELF e_type value to its symbolic name, mirroring pyelftools' ENUM_E_TYPE. */
std::string elfTypeName(std::uint16_t type) {
    switch (type) {
        case ELFIO::ET_NONE: return "ET_NONE";
        case ELFIO::ET_REL: return "ET_REL";
        case ELFIO::ET_EXEC: return "ET_EXEC";
        case ELFIO::ET_DYN: return "ET_DYN";
        case ELFIO::ET_CORE: return "ET_CORE";
        default: return std::to_string(type);
    }
}

/** @brief Maps an ELF e_machine value to its symbolic name, mirroring pyelftools' EM_CUDA entry. */
std::string elfMachineName(std::uint16_t machine) {
    constexpr std::uint16_t EM_CUDA = 190;
    if (machine == EM_CUDA) {
        return "EM_CUDA";
    }
    return std::to_string(machine);
}

/** @brief Maps an ELF sh_type value to its symbolic name, mirroring pyelftools' ENUM_SH_TYPE. */
std::string shTypeName(std::uint32_t type) {
    switch (type) {
        case ELFIO::SHT_NULL: return "SHT_NULL";
        case ELFIO::SHT_PROGBITS: return "SHT_PROGBITS";
        case ELFIO::SHT_SYMTAB: return "SHT_SYMTAB";
        case ELFIO::SHT_STRTAB: return "SHT_STRTAB";
        case ELFIO::SHT_RELA: return "SHT_RELA";
        case ELFIO::SHT_HASH: return "SHT_HASH";
        case ELFIO::SHT_DYNAMIC: return "SHT_DYNAMIC";
        case ELFIO::SHT_NOTE: return "SHT_NOTE";
        case ELFIO::SHT_NOBITS: return "SHT_NOBITS";
        case ELFIO::SHT_REL: return "SHT_REL";
        case ELFIO::SHT_SHLIB: return "SHT_SHLIB";
        case ELFIO::SHT_DYNSYM: return "SHT_DYNSYM";
        default: return std::to_string(type);
    }
}

/** @brief Maps an ELF p_type value to its symbolic name, mirroring pyelftools' ENUM_P_TYPE. */
std::string segTypeName(std::uint32_t type) {
    switch (type) {
        case ELFIO::PT_NULL: return "PT_NULL";
        case ELFIO::PT_LOAD: return "PT_LOAD";
        case ELFIO::PT_DYNAMIC: return "PT_DYNAMIC";
        case ELFIO::PT_INTERP: return "PT_INTERP";
        case ELFIO::PT_NOTE: return "PT_NOTE";
        case ELFIO::PT_SHLIB: return "PT_SHLIB";
        case ELFIO::PT_PHDR: return "PT_PHDR";
        default: return std::to_string(type);
    }
}

} // namespace

CubinFile::CubinFile(const std::string& cubinName) : m_CubinName(cubinName) {
    loadCubin(cubinName);
}

CubinFile::~CubinFile() = default;

void CubinFile::reset() {
    m_ELFFileHeader = ELFIO::Elf64_Ehdr{};
    m_ELFSectionOrder.clear();
    m_ELFSections.clear();
    m_ELFSegments.clear();
    m_ElfReader.reset();

    m_AsmLines.clear();
    m_AsmSectionMarkers.clear();

    m_Arch.reset();
    m_VirtualSMVersion = 0;
    m_ToolKitVersion = 0;
}

void CubinFile::loadCubin(const std::string& binName) {
    CuAsmLogger::logTimeIt("CubinFile::loadCubin", [this, &binName] {
        CuAsmLogger::logEntry("Loading cubin file " + binName + "...");

        reset();    // Resets all vectors and such to the default state

		// Maps section start offsets to section names, used to find the nearest section for a segment's start.
        std::map<std::uint64_t, std::string> secStartDict;
        
		// Maps section end offsets to section names, used to find the nearest section for a segment's end.
        std::map<std::uint64_t, std::string> secEndDict;

        // Parse the whole cubin (ELF header, section header table, program header table) via
        // ELFIO's high-level loader instead of hand-rolled struct memcpy's. Loaded directly into
        // m_ElfReader (rather than a local later moved into it) because ELFIO's section_impl
        // objects hold raw, non-owning pointers back to their parent elfio's convertor/translator
        // members; moving the elfio object leaves every section pointing at the moved-from
        // object's now-destroyed storage.
        m_ElfReader = std::make_unique<ELFIO::elfio>();
        if (!m_ElfReader->load(binName)) {
            throw std::runtime_error("Cannot open or parse cubin file " + binName + " as ELF!");
        }

        
        m_ELFFileHeader.e_ident[ELFIO::EI_OSABI] = m_ElfReader->get_os_abi();
        m_ELFFileHeader.e_ident[ELFIO::EI_ABIVERSION] = m_ElfReader->get_abi_version();
        m_ELFFileHeader.e_type = m_ElfReader->get_type();
        m_ELFFileHeader.e_machine = m_ElfReader->get_machine();
        m_ELFFileHeader.e_version = m_ElfReader->get_version();
        m_ELFFileHeader.e_entry = m_ElfReader->get_entry();
        m_ELFFileHeader.e_phoff = m_ElfReader->get_segments_offset();
        m_ELFFileHeader.e_shoff = m_ElfReader->get_sections_offset();
        m_ELFFileHeader.e_flags = m_ElfReader->get_flags();
        m_ELFFileHeader.e_ehsize = m_ElfReader->get_header_size();
        m_ELFFileHeader.e_phentsize = m_ElfReader->get_segment_entry_size();
        m_ELFFileHeader.e_phnum = static_cast<ELFIO::Elf_Half>(m_ElfReader->segments.size());
        m_ELFFileHeader.e_shentsize = m_ElfReader->get_section_entry_size();
        m_ELFFileHeader.e_shnum = static_cast<ELFIO::Elf_Half>(m_ElfReader->sections.size());
        m_ELFFileHeader.e_shstrndx = m_ElfReader->get_section_name_str_index();
         
        if (m_ELFFileHeader.e_type != ELFIO::ET_EXEC) {
            CuAsmLogger::logWarning(
                std::format("Currently only ET_EXEC type of elf is supported! {} given...", elfTypeName(m_ELFFileHeader.e_type)));
        } else if (m_ELFFileHeader.e_shoff == m_ELFFileHeader.e_ehsize) {
            const std::string msg = "Abnormal elf layout detected! Section headers directly follow elf header.";
            CuAsmLogger::logWarning(msg);
            throw std::runtime_error(msg);
        } else if (m_ELFFileHeader.e_phoff == 0 || m_ELFFileHeader.e_phnum == 0) {
            CuAsmLogger::logWarning("Abnormal elf layout detected! No program header found!");
        }

        const std::uint32_t vsmVersion = (m_ELFFileHeader.e_flags >> 16) & 0xff;
        const std::uint32_t smVersion = m_ELFFileHeader.e_flags & 0xff;
        m_Arch.emplace(static_cast<int>(smVersion));
        m_VirtualSMVersion = static_cast<int>(vsmVersion);
        m_ToolKitVersion = m_ELFFileHeader.e_version;

        std::uint64_t shIndex = m_ELFFileHeader.e_ehsize;
        std::vector<std::tuple<std::uint64_t, std::uint64_t, std::string>> shEdgeList;

        for (int isec = 0; isec < static_cast<int>(m_ElfReader->sections.size()); ++isec) {
            const ELFIO::section* elfioSection = m_ElfReader->sections[isec];
            const std::string secName = elfioSection->get_name();

            ELFIO::Elf64_Shdr shdr{};
            shdr.sh_name = elfioSection->get_name_string_offset();
            shdr.sh_type = elfioSection->get_type();
            shdr.sh_flags = elfioSection->get_flags();
            shdr.sh_addr = elfioSection->get_address();
            shdr.sh_offset = elfioSection->get_offset();
            shdr.sh_size = elfioSection->get_size();
            shdr.sh_link = elfioSection->get_link();
            shdr.sh_info = elfioSection->get_info();
            shdr.sh_addralign = elfioSection->get_addr_align();
            shdr.sh_entsize = elfioSection->get_entry_size();

            CubinElfSection section;
            section.header = shdr;
            if (shdr.sh_type != ELFIO::SHT_NOBITS && shdr.sh_size > 0) {
                const auto rawBytes = std::as_bytes(std::span(elfioSection->get_data(), static_cast<std::size_t>(shdr.sh_size)));
                section.data.assign(rawBytes.begin(), rawBytes.end());
            }
            m_ELFSections[secName] = std::move(section);
            m_ELFSectionOrder.push_back(secName);

            const std::uint64_t shAlign = shdr.sh_addralign;
            const std::uint64_t shSize = shdr.sh_size;
            if (shAlign == 0) {
                shEdgeList.emplace_back(0, 0, secName);
                continue;
            }
            if (shIndex % shAlign != 0) {
                shIndex = ((shIndex + shAlign - 1) / shAlign) * shAlign;
            }

            const std::uint64_t shStart = shIndex;
            const std::uint64_t shEnd = shIndex + shSize;
            shIndex += shSize;
            shEdgeList.emplace_back(shStart, shEnd, secName);
        }

        for (const auto& [shStart, shEnd, sname] : shEdgeList) {
            secStartDict[shStart] = sname;
            secEndDict[shEnd] = sname;
        }

        if (m_ELFFileHeader.e_phnum > 0) {
            const std::uint64_t poff = m_ELFFileHeader.e_phoff;
            secStartDict[poff] = PROGRAM_HEADER_TAG;
            const std::uint64_t pend = poff + static_cast<std::uint64_t>(m_ELFFileHeader.e_phnum) * m_ELFFileHeader.e_phentsize;
            secEndDict[pend] = PROGRAM_HEADER_TAG;
        }

        for (int iseg = 0; iseg < static_cast<int>(m_ElfReader->segments.size()); ++iseg) {
            const ELFIO::segment* elfioSegment = m_ElfReader->segments[iseg];

            ELFIO::Elf64_Phdr segh{};
            segh.p_type = elfioSegment->get_type();
            segh.p_flags = elfioSegment->get_flags();
            segh.p_offset = elfioSegment->get_offset();
            segh.p_vaddr = elfioSegment->get_virtual_address();
            segh.p_paddr = elfioSegment->get_physical_address();
            segh.p_filesz = elfioSegment->get_file_size();
            segh.p_memsz = elfioSegment->get_memory_size();
            segh.p_align = elfioSegment->get_align();

            CubinElfSegment segment;
            segment.header = segh;

            if (segh.p_type == ELFIO::PT_LOAD) {
                const std::uint64_t p0 = segh.p_offset;
                const std::uint64_t p1 = p0 + segh.p_memsz;

                std::string secStart;
                auto itStart = secStartDict.find(p0);
                if (itStart == secStartDict.end()) {
                    CuAsmLogger::logWarning(
                        std::format("The segment start (0x{:x}, 0x{:x}) doesnot align with sections!", p0, p1));
                    CuAsmLogger::logWarning("Try to seek the nearest one...");

                    bool found = false;
                    std::uint64_t maxKey = 0;
                    for (const auto& [k, v] : secStartDict) {
                        if (k < p0 && (!found || k > maxKey)) {
                            maxKey = k;
                            found = true;
                        }
                    }
                    if (!found) {
                        const std::string msg =
                            std::format("Cannot locate start position for segment {} with range (0x{:x}, 0x{:x})!", iseg, p0, p1);
                        CuAsmLogger::logCritical(msg);
                        throw std::runtime_error(msg);
                    }
                    secStart = secStartDict[maxKey];
                } else {
                    secStart = itStart->second;
                }

                std::string secEnd;
                auto itEnd = secEndDict.find(p1);
                if (itEnd == secEndDict.end()) {
                    CuAsmLogger::logWarning(
                        std::format("The segment end (0x{:x}, 0x{:x}) doesnot align with sections!", p0, p1));
                    CuAsmLogger::logWarning("Try to seek the nearest one...");

                    bool found = false;
                    std::uint64_t minKey = 0;
                    for (const auto& [k, v] : secEndDict) {
                        if (k > p1 && (!found || k < minKey)) {
                            minKey = k;
                            found = true;
                        }
                    }
                    if (!found) {
                        const std::string msg =
                            std::format("Cannot locate end position for segment {} with range (0x{:x}, 0x{:x})!", iseg, p0, p1);
                        CuAsmLogger::logCritical(msg);
                        throw std::runtime_error(msg);
                    }
                    secEnd = secEndDict[minKey];
                } else {
                    secEnd = itEnd->second;
                }

                segment.hasRange = true;
                segment.startSection = secStart;
                segment.endSection = secEnd;
            }

            m_ELFSegments.push_back(std::move(segment));
        }

        std::string asmtext;
        if (m_Arch->needsDescHack()) {
            const std::string tmpName = getTempFileName("", "cuasm", "cubin");
            CuAsmLogger::logWarning("This Cubin(" + m_Arch->getVersionString() + ") needs desc hack!");
            fixCubinDesc(binName, tmpName);
            asmtext = CubinFile::disassembleCubin(tmpName);
            std::filesystem::remove(tmpName);
        } else {
            asmtext = CubinFile::disassembleCubin(binName);
        }

        m_AsmLines.clear();
        {
            std::istringstream lineStream(asmtext);
            std::string line;
            while (std::getline(lineStream, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                m_AsmLines.push_back(line);
            }
        }

        m_AsmSectionMarkers = splitAsmSection(m_AsmLines);
    })();
}

void CubinFile::writeFileHeaderAsm(std::ostream& os, const std::string& ident) const {
    CuAsmLogger::logSubroutine("Writing CuAsm file header...");

    const ELFIO::Elf64_Ehdr& fheader = m_ELFFileHeader;
    const auto [m0, m1] = m_AsmSectionMarkers.at("$FileHeader");

    os << ident << "// All file header info is kept as is (unless offset/size attributes)\n";
    os << ident << "// The original header flags is not complete, thus discarded. \n";
    for (int i = m0; i < m1; ++i) {
        os << ident << "// " << m_AsmLines[i] << "\n";
    }

    os << ident << std::format(".__elf_ident_osabi      {}\n", fheader.e_ident[ELFIO::EI_OSABI]);
    os << ident << std::format(".__elf_ident_abiversion {}\n", fheader.e_ident[ELFIO::EI_ABIVERSION]);
    os << ident << std::format(".__elf_type             {}\n", elfTypeName(fheader.e_type));
    os << ident << std::format(".__elf_machine          {}\n", elfMachineName(fheader.e_machine));
    os << ident << std::format(".__elf_version          {} \t\t// CUDA toolkit version \n", fheader.e_version);
    os << ident << std::format(".__elf_entry            {} \t\t// entry point address \n", fheader.e_entry);
    os << ident
       << std::format(".__elf_phoff            0x{:x} \t\t// program header offset, maybe updated by assembler\n", fheader.e_phoff);
    os << ident
       << std::format(".__elf_shoff            0x{:x} \t\t// section header offset, maybe updated by assembler\n", fheader.e_shoff);

    const std::uint32_t vsmversion = (fheader.e_flags >> 16) & 0xff;
    const std::uint32_t smversion = fheader.e_flags & 0xff;
    os << ident
       << std::format(".__elf_flags            0x{:x} \t\t// Flags, SM_{}(0x{:x}), COMPUTE_{}(0x{:x}) \n", fheader.e_flags, smversion,
                       smversion, vsmversion, vsmversion);
    os << ident << std::format(".__elf_ehsize           {} \t\t// elf header size \n", fheader.e_ehsize);
    os << ident << std::format(".__elf_phentsize        {} \t\t// program entry size\n", fheader.e_phentsize);
    os << ident << std::format(".__elf_phnum            {} \t\t// number of program entries\n", fheader.e_phnum);
    os << ident << std::format(".__elf_shentsize        {} \t\t// section entry size\n", fheader.e_shentsize);
    os << ident
       << std::format(".__elf_shnum            {} \t\t// number of sections, currently no sections can be appended/removed\n",
                       fheader.e_shnum);
    os << ident << std::format(".__elf_shstrndx         {} \t\t// Section name string table index \n", fheader.e_shstrndx);
    os << "\n";
}

void CubinFile::writeSectionHeaderAsm(std::ostream& os, const std::string& /*secName*/, const ELFIO::Elf64_Shdr& header,
                                       const std::string& ident) const {
    os << ident << std::format(".__section_name         0x{:x} \t// offset in .shstrtab\n", header.sh_name);
    os << ident << std::format(".__section_type         {}\n", shTypeName(header.sh_type));
    os << ident << std::format(".__section_flags        0x{:x}\n", header.sh_flags);
    os << ident << std::format(".__section_addr         0x{:x}\n", header.sh_addr);
    os << ident << std::format(".__section_offset       0x{:x} \t// maybe updated by assembler\n", header.sh_offset);
    os << ident << std::format(".__section_size         0x{:x} \t// maybe updated by assembler\n", header.sh_size);
    os << ident << std::format(".__section_link         {}\n", header.sh_link);
    os << ident << std::format(".__section_info         0x{:x}\n", header.sh_info);
    os << ident << std::format(".__section_entsize      {}\n", header.sh_entsize);
    os << ident << std::format(".align                {} \t// equivalent to set sh_addralign\n", header.sh_addralign);
}

void CubinFile::writeCodeSectionAsm(std::ostream& os, const std::string& secName) {
    CuAsmLogger::logSubroutine("Writing code section " + secName + "...");

    const auto [mstart, mend] = m_AsmSectionMarkers.at(secName);
    std::vector<std::string> asmlines(m_AsmLines.begin() + mstart, m_AsmLines.begin() + mend);

    std::string kname = std::regex_replace(secName, std::regex("^\\.text\\."), "");
    const std::string nvinfoSecName = ".nv.info." + kname;
    const auto nvinfoIt = m_ELFSections.find(nvinfoSecName);
    if (nvinfoIt == m_ELFSections.end()) {
        throw std::runtime_error("Info section (" + nvinfoSecName + ") not found!");
    }

    const std::vector<std::byte>& nvInfoData = nvinfoIt->second.data;
    CuNVInfo nvinfo(std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(nvInfoData.data()),
                                               reinterpret_cast<const std::uint8_t*>(nvInfoData.data()) + nvInfoData.size()),
                    m_Arch->getVersionString());
    const std::map<std::uint64_t, std::string> offsetLabels = nvinfo.getOffsetLabelDict(kname);

    const CubinElfSection& codeSection = m_ELFSections.at(secName);

    const auto [ctrlCodeList, insCodeList] = m_Arch->splitCtrlCodeFromBytes(codeSection.data);

    static const std::regex mIns(R"(^\s*/\*([0-9a-f]+)\*/\s+.*)");
    static const std::regex pQNAN(R"((\+|-)QNAN\b)");

    int pidx = -1;
    os << asmlines[0] << "\n";
    writeSectionHeaderAsm(os, secName, codeSection.header);

    for (std::size_t i = 1; i < asmlines.size(); ++i) {
        const std::string& line = asmlines[i];
        std::smatch m;
        if (std::regex_search(line, m, mIns)) {
            const std::uint64_t addr = std::stoull(m[1].str(), nullptr, 16);

            const auto labelIt = offsetLabels.find(addr);
            if (labelIt != offsetLabels.end()) {
                os << "  " << labelIt->second << ":\n";
            }

            const int idx = m_Arch->getInsIndexFromOffset(addr);
            if (idx - pidx != 1) {
                CuAsmLogger::logWarning(std::format("!!! Missing instruction before {}:0x{:x}", secName, addr));
            }

            for (int iIns = pidx + 1; iIns < idx; ++iIns) {
                const std::string cstr = CuControlCode::decode(ctrlCodeList[iIns]);
                std::ostringstream icodeHex;
                icodeHex << std::hex << insCodeList[iIns];
                const std::string istr = "    UNDEF 0x" + icodeHex.str() + "; // Missing instructions, not disassembled";
                os << "      [" << cstr << "] " << istr << "\n";
            }

            pidx = idx;

            const std::string cstr = CuControlCode::decode(ctrlCodeList[idx]);

            if (std::regex_search(line, pQNAN)) {
                // The QNAN immediate always lives within the low 64 bits, even for sm7x/8x's
                // wider (up to 105-bit) instruction codes.
                const std::uint64_t low64 = (insCodeList[idx] & ((BigInt(1) << 64) - 1)).convert_to<std::uint64_t>();
                const std::string hline = m_Arch->hackDisassembly(low64, line);
                CuAsmLogger::logWarning("QNAN rewritten in " + secName + " : " + line);
                os << "      [" << cstr << "] " << hline << " // QNAN rewritten: " << line << "\n";
            } else {
                os << "      [" << cstr << "] " << line << "\n";
            }
        } else {
            os << "  " << line << "\n";
        }
    }
}

void CubinFile::writeExplicitSectionAsm(std::ostream& os, const std::string& secName) {
    CuAsmLogger::logSubroutine("Writing explicit section " + secName + "...");

    const auto [m0, m1] = m_AsmSectionMarkers.at(secName);
    os << m_AsmLines[m0] << "\n";

    const CubinElfSection& section = m_ELFSections.at(secName);
    writeSectionHeaderAsm(os, secName, section.header);

    os << "  ";
    for (int i = m0 + 1; i < m1; ++i) {
        if (i > m0 + 1) {
            os << "\n  ";
        }
        os << m_AsmLines[i];
    }
}

void CubinFile::writeImplicitSectionAsm(std::ostream& os, const std::string& secName) {
    CuAsmLogger::logSubroutine("Writing implicit section " + secName + "...");

    const CubinElfSection& section = m_ELFSections.at(secName);
    const ELFIO::Elf64_Shdr& header = section.header;
    const std::span<const std::byte> data = section.data;

    if (secName == ".shstrtab" || secName == ".strtab") {
        os << std::format("\t.section  \"{}\", {}, {}\n", secName, header.sh_flags, shTypeName(header.sh_type));
        os << "\t// all strings in " << secName << " section will be kept as is.\n";
        writeSectionHeaderAsm(os, secName, header);
        os << stringBytes2Asm(data, secName);
    } else if (secName == ".symtab") {
        os << std::format("\t.section  \"{}\", {}, {}\n", secName, header.sh_flags, shTypeName(header.sh_type));
        os << "\t// all symbols in .symtab sections will be kept\n";
        os << "\t// but the symbol size may be changed accordingly\n";
        writeSectionHeaderAsm(os, secName, header);

        const std::uint64_t symEntsize = header.sh_entsize;

        ELFIO::section* symSection = m_ElfReader->sections[secName];
        const ELFIO::symbol_section_accessor symbols(*m_ElfReader, symSection);
        const ELFIO::Elf_Xword nsym = symbols.get_symbols_num();

        for (ELFIO::Elf_Xword isym = 0; isym < nsym; ++isym) {
            std::string symName;
            ELFIO::Elf64_Addr value = 0;
            ELFIO::Elf_Xword size = 0;
            unsigned char bind = 0;
            unsigned char type = 0;
            unsigned char other = 0;
            ELFIO::Elf_Half secIndex = 0;
            symbols.get_symbol(isym, symName, value, size, bind, type, secIndex, other);
            const unsigned char info = ELF_ST_INFO(bind, type);

            os << std::format("    // Symbol[{}] \"{}\": st_value=0x{:x} st_size={} st_info=0x{:x} st_other=0x{:x} st_shndx={}\n", isym,
                               symName, value, size, info, other, secIndex);
            os << bytes2Asm(data.subspan(isym * symEntsize, symEntsize), 8, isym * symEntsize);
            os << "\n";
        }
    } else if (secName.empty()) {
        os << "\t// there will always be an empty section at index 0\n";
        os << std::format("\t.section  \"{}\", {}, {}\n", secName, header.sh_flags, shTypeName(header.sh_type));
        writeSectionHeaderAsm(os, secName, header);
    } else if (secName.starts_with(".rel")) {
        os << std::format("\t.section  \"{}\", {}, {}\n", secName, header.sh_flags, shTypeName(header.sh_type));
        os << "\t// all relocation sections will be dynamically generated by assembler \n";
        os << "\t// but most of the section header will be kept as is.\n";
        writeSectionHeaderAsm(os, secName, header);

        ELFIO::section* relSection = m_ElfReader->sections[secName];
        const ELFIO::relocation_section_accessor relocs(*m_ElfReader, relSection);
        const ELFIO::Elf_Xword nrel = relocs.get_entries_num();

        ELFIO::section* symSection = m_ElfReader->sections[".symtab"];
        std::optional<ELFIO::symbol_section_accessor> symbols;
        if (symSection) {
            symbols.emplace(*m_ElfReader, symSection);
        }

        for (ELFIO::Elf_Xword irel = 0; irel < nrel; ++irel) {
            ELFIO::Elf64_Addr rOffset = 0;
            ELFIO::Elf_Word rSym = 0;
            unsigned rType = 0;
            ELFIO::Elf_Sxword rAddend = 0;
            relocs.get_entry(irel, rOffset, rSym, rType, rAddend);

            std::string symName;
            if (symbols) {
                ELFIO::Elf64_Addr symValue = 0;
                ELFIO::Elf_Xword symSize = 0;
                unsigned char bind = 0;
                unsigned char symType = 0;
                unsigned char other = 0;
                ELFIO::Elf_Half secIndex = 0;
                symbols->get_symbol(rSym, symName, symValue, symSize, bind, symType, secIndex, other);
            }

            os << std::format("    // Relocation[{}] : {}, r_offset=0x{:x} r_type={} r_addend={}\n", irel, symName, rOffset, rType,
                               rAddend);
        }
    } else {
        throw std::runtime_error("Unknown implicit section " + secName + " !");
    }
}

void CubinFile::writeSegmentHeaderAsm(std::ostream& os, const ELFIO::Elf64_Phdr& segHeader, const CubinElfSegment& segRange) const {
    CuAsmLogger::logSubroutine("Writing segment header...");

    os << std::format("// Program segment {}, {} \n", segTypeName(segHeader.p_type), segHeader.p_flags);
    os << std::format("  .__segment  \"{}\", {} \n", segTypeName(segHeader.p_type), segHeader.p_flags);
    os << std::format("  .__segment_offset  0x{:x}   \t\t// maybe updated by assembler \n", segHeader.p_offset);
    os << std::format("  .__segment_vaddr   0x{:x}   \t\t// Seems always 0? \n", segHeader.p_vaddr);
    os << std::format("  .__segment_paddr   0x{:x}   \t\t// ??? \n", segHeader.p_paddr);
    os << std::format("  .__segment_filesz  0x{:x}   \t\t// file size, maybe updated by assembler \n", segHeader.p_filesz);
    os << std::format("  .__segment_memsz   0x{:x}   \t\t// file size + nobits sections, maybe updated by assembler \n",
                       segHeader.p_memsz);
    os << std::format("  .__segment_align     {}   \t\t//  \n", segHeader.p_align);
    if (segRange.hasRange) {
        os << std::format("  .__segment_startsection    \"{}\"  \t\t// first section in this segment \n", segRange.startSection);
        os << std::format("  .__segment_endsection      \"{}\"  \t\t// last  section in this segment \n", segRange.endSection);
    }
    os << "\n";
}

void CubinFile::saveAsCuAsm(const std::string& asmName) {
    CuAsmLogger::logTimeIt("CubinFile::saveAsCuAsm", [this, &asmName] {
        CuAsmLogger::logEntry("Saving to cuasm file " + asmName + "...");

        std::ofstream fout(asmName, std::ios::out | std::ios::trunc);
        if (!fout) {
            throw std::runtime_error("Cannot open " + asmName + " for writing!");
        }

        fout << "// --------------------- FileHeader --------------------------\n";
        writeFileHeaderAsm(fout);

        fout << "\n";
        fout << "  //-------------------------------------------------\n";
        fout << "  //------------ END of FileHeader ------------------\n";
        fout << "  //-------------------------------------------------\n\n\n";

        for (const std::string& secName : m_ELFSectionOrder) {
            fout << std::format("\n// --------------------- {:<32} --------------------------\n", secName);
            if (m_AsmSectionMarkers.count(secName)) {
                if (secName.starts_with(".text.")) {
                    writeCodeSectionAsm(fout, secName);
                } else {
                    writeExplicitSectionAsm(fout, secName);
                }
            } else {
                writeImplicitSectionAsm(fout, secName);
            }
        }

        fout << "\n";
        fout << "  //-------------------------------------------------\n";
        fout << "  //---------------- END of sections ----------------\n";
        fout << "  //-------------------------------------------------\n\n\n";

        for (const CubinElfSegment& segment : m_ELFSegments) {
            writeSegmentHeaderAsm(fout, segment.header, segment);
        }

        fout << "\n";
        fout << "  //-------------------------------------------------\n";
        fout << "  //---------------- END of segments ----------------\n";
        fout << "  //-------------------------------------------------\n\n\n";
    })();
}

std::string CubinFile::disassembleCubin(const std::string& binName) {
    return CuAsmLogger::logTimeIt("CubinFile::disassembleCubin", [](const std::string& name) {
        CuAsmLogger::logProcedure("Disassembling " + name + "...");
        return checkOutput({Config::NVDISASM_PATH, name}, /*mergeStderr=*/false);
    })(binName);
}

} // namespace CuAsm
