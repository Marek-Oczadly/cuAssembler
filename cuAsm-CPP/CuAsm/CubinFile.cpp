#include "CubinFile.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include <elfio/elf_types.hpp>

#include "CuAsmLogger.hpp"
#include "CuControlCode.hpp"
#include "CuNVInfo.hpp"
#include "common.hpp"
#include "config.hpp"
#include "utils/CubinUtils.hpp"

namespace CuAsm {

namespace {

/** @brief Reads a NUL-terminated string out of a string table at a given byte offset. */
std::string readCString(const std::string& strtab, std::uint32_t offset) {
    if (offset >= strtab.size()) {
        return "";
    }
    const std::size_t end = strtab.find('\0', offset);
    return strtab.substr(offset, (end == std::string::npos ? strtab.size() : end) - offset);
}

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

/** @brief printf-style formatting into a std::string, used to mirror python's '%' formatting. */
std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list argsCopy;
    va_copy(argsCopy, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, argsCopy);
    va_end(argsCopy);

    if (needed < 0) {
        va_end(args);
        return std::string();
    }

    std::string result(static_cast<std::size_t>(needed), '\0');
    std::vsnprintf(result.data(), static_cast<std::size_t>(needed) + 1, fmt, args);
    va_end(args);
    return result;
}

} // namespace

CubinFile::CubinFile(const std::string& cubinName) : m_CubinName(cubinName) {
    loadCubin(cubinName);
}

void CubinFile::reset() {
    m_ELFFileHeader = ELFIO::Elf64_Ehdr{};
    m_ELFSectionOrder.clear();
    m_ELFSections.clear();
    m_ELFSegments.clear();

    m_AsmLines.clear();
    m_AsmSectionMarkers.clear();
    m_CubinBytes.clear();

    m_Arch.reset();
    m_VirtualSMVersion = 0;
    m_ToolKitVersion = 0;
}

void CubinFile::loadCubin(const std::string& binName) {
    CuAsmLogger::logTimeIt("CubinFile::loadCubin", [this, &binName] {
        CuAsmLogger::logEntry("Loading cubin file " + binName + "...");

        reset();

        std::map<std::uint64_t, std::string> secStartDict;
        std::map<std::uint64_t, std::string> secEndDict;

        {
            std::ifstream fin(binName, std::ios::binary);
            if (!fin) {
                throw std::runtime_error("Cannot open cubin file " + binName);
            }
            std::ostringstream ss;
            ss << fin.rdbuf();
            m_CubinBytes = ss.str();
        }

        if (m_CubinBytes.size() < sizeof(ELFIO::Elf64_Ehdr)) {
            throw std::runtime_error("Cubin file " + binName + " is too small to contain an ELF header!");
        }
        std::memcpy(&m_ELFFileHeader, m_CubinBytes.data(), sizeof(m_ELFFileHeader));

        if (m_ELFFileHeader.e_type != ELFIO::ET_EXEC) {
            CuAsmLogger::logWarning(format("Currently only ET_EXEC type of elf is supported! %s given...",
                                            elfTypeName(m_ELFFileHeader.e_type).c_str()));
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

        // parse raw section header table
        std::vector<ELFIO::Elf64_Shdr> rawShdrs(m_ELFFileHeader.e_shnum);
        for (std::uint16_t i = 0; i < m_ELFFileHeader.e_shnum; ++i) {
            const std::size_t off = m_ELFFileHeader.e_shoff + static_cast<std::size_t>(i) * m_ELFFileHeader.e_shentsize;
            std::memcpy(&rawShdrs[i], m_CubinBytes.data() + off, sizeof(ELFIO::Elf64_Shdr));
        }

        const ELFIO::Elf64_Shdr& shstrtabHdr = rawShdrs[m_ELFFileHeader.e_shstrndx];
        const std::string shstrtabData = m_CubinBytes.substr(shstrtabHdr.sh_offset, shstrtabHdr.sh_size);

        std::uint64_t shIndex = m_ELFFileHeader.e_ehsize;
        std::vector<std::tuple<std::uint64_t, std::uint64_t, std::string>> shEdgeList;

        for (std::uint16_t isec = 0; isec < m_ELFFileHeader.e_shnum; ++isec) {
            const ELFIO::Elf64_Shdr& shdr = rawShdrs[isec];
            const std::string secName = readCString(shstrtabData, shdr.sh_name);

            CubinElfSection section;
            section.header = shdr;
            if (shdr.sh_type != ELFIO::SHT_NOBITS) {
                section.data = m_CubinBytes.substr(shdr.sh_offset, shdr.sh_size);
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

        std::vector<ELFIO::Elf64_Phdr> rawPhdrs(m_ELFFileHeader.e_phnum);
        for (std::uint16_t i = 0; i < m_ELFFileHeader.e_phnum; ++i) {
            const std::size_t off = m_ELFFileHeader.e_phoff + static_cast<std::size_t>(i) * m_ELFFileHeader.e_phentsize;
            std::memcpy(&rawPhdrs[i], m_CubinBytes.data() + off, sizeof(ELFIO::Elf64_Phdr));
        }

        for (std::size_t iseg = 0; iseg < rawPhdrs.size(); ++iseg) {
            const ELFIO::Elf64_Phdr& segh = rawPhdrs[iseg];
            CubinElfSegment segment;
            segment.header = segh;

            if (segh.p_type == ELFIO::PT_LOAD) {
                const std::uint64_t p0 = segh.p_offset;
                const std::uint64_t p1 = p0 + segh.p_memsz;

                std::string secStart;
                auto itStart = secStartDict.find(p0);
                if (itStart == secStartDict.end()) {
                    CuAsmLogger::logWarning(
                        format("The segment start (0x%llx, 0x%llx) doesnot align with sections!",
                               static_cast<unsigned long long>(p0), static_cast<unsigned long long>(p1)));
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
                        const std::string msg = format("Cannot locate start position for segment %u with range (0x%llx, 0x%llx)!",
                                                         static_cast<unsigned>(iseg), static_cast<unsigned long long>(p0),
                                                         static_cast<unsigned long long>(p1));
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
                        format("The segment end (0x%llx, 0x%llx) doesnot align with sections!",
                               static_cast<unsigned long long>(p0), static_cast<unsigned long long>(p1)));
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
                        const std::string msg = format("Cannot locate end position for segment %u with range (0x%llx, 0x%llx)!",
                                                         static_cast<unsigned>(iseg), static_cast<unsigned long long>(p0),
                                                         static_cast<unsigned long long>(p1));
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

    os << ident << format(".__elf_ident_osabi      %d\n", fheader.e_ident[ELFIO::EI_OSABI]);
    os << ident << format(".__elf_ident_abiversion %d\n", fheader.e_ident[ELFIO::EI_ABIVERSION]);
    os << ident << format(".__elf_type             %s\n", elfTypeName(fheader.e_type).c_str());
    os << ident << format(".__elf_machine          %s\n", elfMachineName(fheader.e_machine).c_str());
    os << ident << format(".__elf_version          %u \t\t// CUDA toolkit version \n", fheader.e_version);
    os << ident << format(".__elf_entry            %llu \t\t// entry point address \n",
                          static_cast<unsigned long long>(fheader.e_entry));
    os << ident << format(".__elf_phoff            0x%llx \t\t// program header offset, maybe updated by assembler\n",
                          static_cast<unsigned long long>(fheader.e_phoff));
    os << ident << format(".__elf_shoff            0x%llx \t\t// section header offset, maybe updated by assembler\n",
                          static_cast<unsigned long long>(fheader.e_shoff));

    const std::uint32_t vsmversion = (fheader.e_flags >> 16) & 0xff;
    const std::uint32_t smversion = fheader.e_flags & 0xff;
    os << ident
       << format(".__elf_flags            0x%x \t\t// Flags, SM_%u(0x%x), COMPUTE_%u(0x%x) \n", fheader.e_flags, smversion,
                  smversion, vsmversion, vsmversion);
    os << ident << format(".__elf_ehsize           %u \t\t// elf header size \n", fheader.e_ehsize);
    os << ident << format(".__elf_phentsize        %u \t\t// program entry size\n", fheader.e_phentsize);
    os << ident << format(".__elf_phnum            %u \t\t// number of program entries\n", fheader.e_phnum);
    os << ident << format(".__elf_shentsize        %u \t\t// section entry size\n", fheader.e_shentsize);
    os << ident
       << format(".__elf_shnum            %u \t\t// number of sections, currently no sections can be appended/removed\n",
                  fheader.e_shnum);
    os << ident << format(".__elf_shstrndx         %u \t\t// Section name string table index \n", fheader.e_shstrndx);
    os << "\n";
}

void CubinFile::writeSectionHeaderAsm(std::ostream& os, const std::string& /*secName*/, const ELFIO::Elf64_Shdr& header,
                                       const std::string& ident) const {
    os << ident << format(".__section_name         0x%x \t// offset in .shstrtab\n", header.sh_name);
    os << ident << format(".__section_type         %s\n", shTypeName(header.sh_type).c_str());
    os << ident << format(".__section_flags        0x%llx\n", static_cast<unsigned long long>(header.sh_flags));
    os << ident << format(".__section_addr         0x%llx\n", static_cast<unsigned long long>(header.sh_addr));
    os << ident
       << format(".__section_offset       0x%llx \t// maybe updated by assembler\n", static_cast<unsigned long long>(header.sh_offset));
    os << ident
       << format(".__section_size         0x%llx \t// maybe updated by assembler\n", static_cast<unsigned long long>(header.sh_size));
    os << ident << format(".__section_link         %u\n", header.sh_link);
    os << ident << format(".__section_info         0x%x\n", header.sh_info);
    os << ident << format(".__section_entsize      %llu\n", static_cast<unsigned long long>(header.sh_entsize));
    os << ident << format(".align                %llu \t// equivalent to set sh_addralign\n",
                          static_cast<unsigned long long>(header.sh_addralign));
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

    CuNVInfo nvinfo(std::vector<std::uint8_t>(nvinfoIt->second.data.begin(), nvinfoIt->second.data.end()),
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
                CuAsmLogger::logWarning(format("!!! Missing instruction before %s:0x%llx", secName.c_str(),
                                               static_cast<unsigned long long>(addr)));
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
    const std::string& data = section.data;

    if (secName == ".shstrtab" || secName == ".strtab") {
        os << format("\t.section  \"%s\", %llu, %s\n", secName.c_str(), static_cast<unsigned long long>(header.sh_flags),
                      shTypeName(header.sh_type).c_str());
        os << "\t// all strings in " << secName << " section will be kept as is.\n";
        writeSectionHeaderAsm(os, secName, header);
        os << stringBytes2Asm(data, secName);
    } else if (secName == ".symtab") {
        os << format("\t.section  \"%s\", %llu, %s\n", secName.c_str(), static_cast<unsigned long long>(header.sh_flags),
                      shTypeName(header.sh_type).c_str());
        os << "\t// all symbols in .symtab sections will be kept\n";
        os << "\t// but the symbol size may be changed accordingly\n";
        writeSectionHeaderAsm(os, secName, header);

        const std::uint64_t symEntsize = header.sh_entsize;
        const std::uint64_t nsym = symEntsize == 0 ? 0 : header.sh_size / symEntsize;
        const std::string strtabData = m_ELFSections.count(".strtab") ? m_ELFSections.at(".strtab").data : std::string();

        for (std::uint64_t isym = 0; isym < nsym; ++isym) {
            ELFIO::Elf64_Sym sym{};
            std::memcpy(&sym, data.data() + isym * symEntsize, sizeof(sym));
            const std::string symName = readCString(strtabData, sym.st_name);

            os << format("    // Symbol[%llu] \"%s\": st_value=0x%llx st_size=%llu st_info=0x%x st_other=0x%x st_shndx=%u\n",
                          static_cast<unsigned long long>(isym), symName.c_str(), static_cast<unsigned long long>(sym.st_value),
                          static_cast<unsigned long long>(sym.st_size), sym.st_info, sym.st_other, sym.st_shndx);
            os << bytes2Asm(data.substr(isym * symEntsize, symEntsize), 8, isym * symEntsize);
            os << "\n";
        }
    } else if (secName.empty()) {
        os << "\t// there will always be an empty section at index 0\n";
        os << format("\t.section  \"%s\", %llu, %s\n", secName.c_str(), static_cast<unsigned long long>(header.sh_flags),
                      shTypeName(header.sh_type).c_str());
        writeSectionHeaderAsm(os, secName, header);
    } else if (secName.rfind(".rel", 0) == 0) {
        os << format("\t.section  \"%s\", %llu, %s\n", secName.c_str(), static_cast<unsigned long long>(header.sh_flags),
                      shTypeName(header.sh_type).c_str());
        os << "\t// all relocation sections will be dynamically generated by assembler \n";
        os << "\t// but most of the section header will be kept as is.\n";
        writeSectionHeaderAsm(os, secName, header);

        const bool isRela = header.sh_entsize >= sizeof(ELFIO::Elf64_Rela);
        const std::uint64_t relEntsize = header.sh_entsize;
        const std::uint64_t nrel = relEntsize == 0 ? 0 : header.sh_size / relEntsize;

        const std::string symtabData = m_ELFSections.count(".symtab") ? m_ELFSections.at(".symtab").data : std::string();
        const std::string strtabData = m_ELFSections.count(".strtab") ? m_ELFSections.at(".strtab").data : std::string();
        const std::uint64_t symEntsize = m_ELFSections.count(".symtab") ? m_ELFSections.at(".symtab").header.sh_entsize : 0;

        for (std::uint64_t irel = 0; irel < nrel; ++irel) {
            std::uint64_t rOffset = 0;
            std::uint64_t rInfo = 0;
            std::int64_t rAddend = 0;

            if (isRela) {
                ELFIO::Elf64_Rela rela{};
                std::memcpy(&rela, data.data() + irel * relEntsize, sizeof(rela));
                rOffset = rela.r_offset;
                rInfo = rela.r_info;
                rAddend = rela.r_addend;
            } else {
                ELFIO::Elf64_Rel rel{};
                std::memcpy(&rel, data.data() + irel * relEntsize, sizeof(rel));
                rOffset = rel.r_offset;
                rInfo = rel.r_info;
            }

            const std::uint32_t rSym = static_cast<std::uint32_t>(rInfo >> 32);
            const std::uint32_t rType = static_cast<std::uint32_t>(rInfo & 0xffffffffu);

            std::string symName;
            if (symEntsize != 0 && static_cast<std::uint64_t>(rSym) * symEntsize < symtabData.size()) {
                ELFIO::Elf64_Sym sym{};
                std::memcpy(&sym, symtabData.data() + rSym * symEntsize, sizeof(sym));
                symName = readCString(strtabData, sym.st_name);
            }

            os << format("    // Relocation[%llu] : %s, r_offset=0x%llx r_type=%u r_addend=%lld\n",
                          static_cast<unsigned long long>(irel), symName.c_str(), static_cast<unsigned long long>(rOffset), rType,
                          static_cast<long long>(rAddend));
        }
    } else {
        throw std::runtime_error("Unknown implicit section " + secName + " !");
    }
}

void CubinFile::writeSegmentHeaderAsm(std::ostream& os, const ELFIO::Elf64_Phdr& segHeader, const CubinElfSegment& segRange) const {
    CuAsmLogger::logSubroutine("Writing segment header...");

    os << format("// Program segment %s, %u \n", segTypeName(segHeader.p_type).c_str(), segHeader.p_flags);
    os << format("  .__segment  \"%s\", %u \n", segTypeName(segHeader.p_type).c_str(), segHeader.p_flags);
    os << format("  .__segment_offset  0x%llx   \t\t// maybe updated by assembler \n", static_cast<unsigned long long>(segHeader.p_offset));
    os << format("  .__segment_vaddr   0x%llx   \t\t// Seems always 0? \n", static_cast<unsigned long long>(segHeader.p_vaddr));
    os << format("  .__segment_paddr   0x%llx   \t\t// ??? \n", static_cast<unsigned long long>(segHeader.p_paddr));
    os << format("  .__segment_filesz  0x%llx   \t\t// file size, maybe updated by assembler \n",
                 static_cast<unsigned long long>(segHeader.p_filesz));
    os << format("  .__segment_memsz   0x%llx   \t\t// file size + nobits sections, maybe updated by assembler \n",
                 static_cast<unsigned long long>(segHeader.p_memsz));
    os << format("  .__segment_align     %llu   \t\t//  \n", static_cast<unsigned long long>(segHeader.p_align));
    if (segRange.hasRange) {
        os << format("  .__segment_startsection    \"%s\"  \t\t// first section in this segment \n", segRange.startSection.c_str());
        os << format("  .__segment_endsection      \"%s\"  \t\t// last  section in this segment \n", segRange.endSection.c_str());
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
            fout << format("\n// --------------------- %-32s --------------------------\n", secName.c_str());
            if (m_AsmSectionMarkers.count(secName)) {
                if (secName.rfind(".text.", 0) == 0) {
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
