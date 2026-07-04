#pragma once

#include <cstdint>
#include <string>

#include <elfio/elf_types.hpp>

namespace CuAsm {

/**
 * @brief Global configuration and shared ELF defaults for the assembler, mirroring
 *        CuAsm.config.Config. Currently only little-endian ELFCLASS64 cubins are supported.
 **/
class Config {
public:
    /// Default path to nvdisasm.
    static const std::string NVDISASM_PATH;

    /**
     * @brief Default cubin ELF file header, parsed from a hard-coded reference byte sequence,
     *        mirroring Config.defaultCubinFileHeader.
     * @return The parsed default Elf64_Ehdr.
     **/
    static const ELFIO::Elf64_Ehdr& defaultCubinFileHeader();

    /**
     * @brief Zero-initialized default ELF section header, mirroring Config.defaultSectionHeader.
     * @return The default (all-zero) Elf64_Shdr.
     **/
    static const ELFIO::Elf64_Shdr& defaultSectionHeader();

    /**
     * @brief Zero-initialized default ELF segment header, mirroring Config.defaultSegmentHeader.
     * @return The default (all-zero) Elf64_Phdr.
     **/
    static const ELFIO::Elf64_Phdr& defaultSegmentHeader();

    /**
     * @brief Zero-initialized default ELF symbol table entry, mirroring Config.defaultSymbol.
     * @return The default (all-zero) Elf64_Sym.
     **/
    static const ELFIO::Elf64_Sym& defaultSymbol();

    /**
     * @brief Zero-initialized default ELF relocation entry, mirroring Config.defaultRel.
     * @return The default (all-zero) Elf64_Rel.
     **/
    static const ELFIO::Elf64_Rel& defaultRel();

    /**
     * @brief Zero-initialized default ELF relocation-with-addend entry, mirroring
     *        Config.defaultRela.
     * @return The default (all-zero) Elf64_Rela.
     **/
    static const ELFIO::Elf64_Rela& defaultRela();

    /**
     * @brief Loads configuration from a file. Currently unimplemented, mirroring the TODO left in
     *        Config.load.
     **/
    void load();

    /**
     * @brief Saves configuration to a file. Currently unimplemented, mirroring the TODO left in
     *        Config.save.
     **/
    void save();

    /**
     * @brief Gets the path of the default instruction-assembler repository file for an SM version,
     *        mirroring Config.getDefaultInsAsmReposFile.
     * @param versionNumber SM version number, e.g. 75 for sm_75.
     * @return Path to "InsAsmRepos/DefaultInsAsmRepos.sm_<versionNumber>.txt", next to this module.
     **/
    static std::string getDefaultInsAsmReposFile(int versionNumber);

    /**
     * @brief Gets the path of the default instruction I/O info file for an SM version, falling
     *        back to the shared "IOInfo.all.json" if no per-version file exists, mirroring
     *        Config.getDefaultIOInfoFile.
     * @param versionNumber SM version number, e.g. 75 for sm_75.
     * @return Path to "InsAsmRepos/IOInfo.sm_<versionNumber>.txt" if it exists, else to
     *         "InsAsmRepos/IOInfo.all.json".
     **/
    static std::string getDefaultIOInfoFile(int versionNumber);
};

} // namespace CuAsm
