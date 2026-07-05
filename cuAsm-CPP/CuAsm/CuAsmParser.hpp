#pragma once

#include <memory>
#include <string>

namespace CuAsm {

/**
 * @brief Parser for .cuasm files, mirroring CuAsm.CuAsmParser.CuAsmParser.
 *
 * General parsing work flow (see parse()):
 * - scan whole file, gathering file headers, section headers/contents, segment headers,
 *   build fixup lists, split kernel text sections for kernel assembler.
 * - build internal tables, such as .shstrtab, .strtab, .symtab (currently just copied except
 *   symbol size).
 * - build kernel text sections, update .nv.info sections if necessary, update relocations if
 *   there are any.
 * - evaluate fixups, patching the bytes of corresponding section data.
 * - build relocation sections.
 * - layout sections, update file header, section header, segment header accordingly.
 * - write to file/stream.
 *
 * All the helper types the original module defines at file scope (CuAsmSymbol, CuAsmLabel,
 * CuAsmFixup, CuAsmSection, CuAsmSegment, CuAsmRelocation, CuAsmFile) are internal
 * implementation details of this class here, hidden behind a pimpl, since nothing outside this
 * file references them.
 **/
class CuAsmParser {
public:
    CuAsmParser();
    ~CuAsmParser();

    CuAsmParser(const CuAsmParser&) = delete;
    CuAsmParser& operator=(const CuAsmParser&) = delete;
    CuAsmParser(CuAsmParser&&) noexcept;
    CuAsmParser& operator=(CuAsmParser&&) noexcept;

    /**
     * @brief Sets the instruction-assembler repository used to encode kernel text sections.
     * @param fname Path of a previously saved repos file.
     * @param arch Arch string, e.g. "sm_75".
     **/
    void setInsAsmRepos(const std::string& fname, const std::string& arch);

    /**
     * @brief Parses a .cuasm source file, populating internal state for assembly.
     * @param fname Path of the .cuasm file to parse.
     **/
    void parse(const std::string& fname);

    /**
     * @brief Assembles the previously parsed source and writes it out as a .cubin file.
     * @param binFileName Path of the .cubin file to write.
     **/
    void saveAsCubin(const std::string& binFileName);

    /**
     * @brief Debug helper: writes the currently assembled contents and the given reference
     *        cubin's contents (file header, section headers/data) side-by-side to
     *        "<savPrefix>_asm.txt" and "<savPrefix>_bin.txt" for comparison.
     * @param cubinName Path of the reference .cubin file to compare against.
     * @param savPrefix Prefix used to build the two output text file names.
     **/
    void saveCubinCmp(const std::string& cubinName, const std::string& savPrefix);

    /** @brief Prints the fixup list gathered so far to stdout. */
    void dispFixupList() const;

    /** @brief Prints the relocation list gathered so far to stdout. */
    void dispRelocationList() const;

    /** @brief Prints the currently assembled cubin's section list (offset/size/type/flags/name) to stdout. */
    void dispSectionList() const;

    /** @brief Prints the explicitly-defined (.type/.size/.global/.weak/.other) symbols to stdout. */
    void dispSymbolDict() const;

    /** @brief Prints the .symtab entries (and any matching explicitly-defined symbol) to stdout. */
    void dispSymtabDict() const;

    /** @brief Prints the label table to stdout. */
    void dispLabelDict() const;

    /** @brief Prints the segment headers to stdout. */
    void dispSegmentHeader() const;

    /** @brief Prints the ELF file header attributes to stdout. */
    void dispFileHeader() const;

    /** @brief Prints the .shstrtab/.strtab/.symtab tables to stdout. */
    void dispTables() const;

    /**
     * @brief Strips cpp-style, c-style and bra-target comments from a line and collapses
     *        whitespace, mirroring CuAsmParser.stripComments.
     * @param s Line to strip.
     * @return The comment-free, whitespace-collapsed, trimmed line.
     **/
    static std::string stripComments(const std::string& s);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace CuAsm
