#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <elfio/elf_types.hpp>

#include "CuSMVersion.hpp"

namespace CuAsm {

/// Pseudo section name used to track the ELF program-header table's own range during
/// section/segment layout, mirroring CubinFile.PROGRAM_HEADER_TAG.
inline constexpr const char* PROGRAM_HEADER_TAG = "@PROGRAM_HEADER";

/// One decoded ELF section: its header plus raw contents, mirroring the (header, data) tuples
/// kept in CubinFile.py's __mELFSections OrderedDict.
struct CubinElfSection {
    ELFIO::Elf64_Shdr header{};
    std::string data;
};

/// One decoded ELF program-header entry plus the [start, end) section names it spans (only set
/// for PT_LOAD segments), mirroring CubinFile.py's parallel __mELFSegments/__mELFSegmentRange
/// lists.
struct CubinElfSegment {
    ELFIO::Elf64_Phdr header{};
    bool hasRange = false;
    std::string startSection;
    std::string endSection;
};

/**
 * @brief CubinFile class for cubin files, mainly used for saving as cuasm, mirroring
 *        CuAsm.CubinFile.CubinFile.
 **/
class CubinFile {
public:
    /**
     * @brief Constructs a CubinFile by loading and disassembling a cubin.
     * @param cubinName Path to the input cubin file.
     **/
    explicit CubinFile(const std::string& cubinName);

    /**
     * @brief Saving current cubin as cuasm file, mirroring CubinFile.saveAsCuAsm.
     * @param asmName Output .cuasm file path.
     **/
    void saveAsCuAsm(const std::string& asmName);

    /**
     * @brief Gets the disassembly of an input file from nvdisasm, mirroring
     *        CubinFile.disassembleCubin.
     * @param binName Path to the cubin file to disassemble.
     * @return The raw nvdisasm output text.
     **/
    static std::string disassembleCubin(const std::string& binName);

    /// Numeric SM version of this cubin's target architecture, e.g. 86 for sm_86.
    std::optional<CuSMVersion> m_Arch;
    /// Virtual (compute_XX) SM version encoded in the ELF header flags.
    int m_VirtualSMVersion = 0;
    /// CUDA toolkit version encoded in the ELF header's e_version field.
    std::uint32_t m_ToolKitVersion = 0;

private:
    /**
     * @brief Loads a cubin file: parses its ELF layout and disassembles it, mirroring
     *        CubinFile.loadCubin.
     * @param binName Path to the input cubin file.
     **/
    void loadCubin(const std::string& binName);

    /**
     * @brief Resets all per-cubin state, mirroring CubinFile.__reset.
     **/
    void reset();

    /**
     * @brief Generates file header asm, mirroring CubinFile.__writeFileHeaderAsm.
     * @param os Stream to write to.
     * @param ident Indentation prefix for each line.
     **/
    void writeFileHeaderAsm(std::ostream& os, const std::string& ident = "\t") const;

    /**
     * @brief Generates section header assembly according to header, mirroring
     *        CubinFile.__writeSectionHeaderAsm.
     * @param os Stream to write to.
     * @param secName Section name (used only for context, not written here).
     * @param header Section header to describe.
     * @param ident Indentation prefix for each line.
     **/
    void writeSectionHeaderAsm(std::ostream& os, const std::string& secName, const ELFIO::Elf64_Shdr& header,
                                const std::string& ident = "\t") const;

    /**
     * @brief Rewrites a code (.text.*) section in assembly, adding control codes and offset
     *        labels, mirroring CubinFile.__writeCodeSectionAsm.
     * @param os Stream to write to.
     * @param secName Name of the .text.* section to write.
     **/
    void writeCodeSectionAsm(std::ostream& os, const std::string& secName);

    /**
     * @brief Writes a section explicitly present in nvdisasm output, mirroring
     *        CubinFile.__writeExplicitSectionAsm.
     * @param os Stream to write to.
     * @param secName Section name to write.
     **/
    void writeExplicitSectionAsm(std::ostream& os, const std::string& secName);

    /**
     * @brief Writes a section not shown in nvdisasm output (.shstrtab/.strtab/.symtab/.rel*\/
     *        etc.), mirroring CubinFile.__writeImplicitSectionAsm.
     * @param os Stream to write to.
     * @param secName Section name to write.
     **/
    void writeImplicitSectionAsm(std::ostream& os, const std::string& secName);

    /**
     * @brief Writes a program segment header, mirroring CubinFile.__writeSegmentHeaderAsm.
     * @param os Stream to write to.
     * @param segHeader Segment (program) header to describe.
     * @param segRange Segment's [start, end) section name range, if any.
     **/
    void writeSegmentHeaderAsm(std::ostream& os, const ELFIO::Elf64_Phdr& segHeader, const CubinElfSegment& segRange) const;

    std::string m_CubinName;

    ELFIO::Elf64_Ehdr m_ELFFileHeader{};
    std::vector<std::string> m_ELFSectionOrder;
    std::map<std::string, CubinElfSection> m_ELFSections;
    std::vector<CubinElfSegment> m_ELFSegments;

    std::string m_CubinBytes;

    std::vector<std::string> m_AsmLines;
    std::map<std::string, std::pair<int, int>> m_AsmSectionMarkers;
};

} // namespace CuAsm
