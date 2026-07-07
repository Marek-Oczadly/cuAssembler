#include "CuAsmParser.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <variant>
#include <vector>

#include <elfio/elfio.hpp>

#include "CuAsmLogger.hpp"
#include "CuControlCode.hpp"
#include "CuInsAssemblerRepos.hpp"
#include "CuKernelAssembler.hpp"
#include "CuNVInfo.hpp"
#include "CuSMVersion.hpp"
#include "CubinFile.hpp"
#include "common.hpp"
#include "config.hpp"

namespace CuAsm {

namespace {

/// A per-attribute header value: either a resolved integer or a (possibly symbolic) string,
/// mirroring the mixed int/str values CuAsmParser keeps in its python `header`/`fileHeader` dicts.
using HeaderValue = std::variant<std::int64_t, std::string>;

/**
 * @brief Ordered string-keyed container preserving insertion order, mirroring python's
 *        OrderedDict as used throughout CuAsmParser.
 */
template <typename T>
class OrderedDict {
public:
    /** @brief Finds or default-inserts an entry by key. @param key Key. @return Reference to the value. */
    T& operator[](const std::string& key) {
        auto it = m_index.find(key);
        if (it != m_index.end()) {
            return m_items[it->second].second;
        }
        m_index.emplace(key, m_items.size());
        m_items.emplace_back(key, T{});
        return m_items.back().second;
    }

    /** @brief Checks whether a key exists. @param key Key. @return True if present. */
    bool contains(const std::string& key) const { return m_index.count(key) != 0; }

    /** @brief Gets an existing entry by key. @param key Key. @return Reference to the value. */
    T& at(const std::string& key) { return m_items[m_index.at(key)].second; }
    /** @brief Const overload of at(). @param key Key. @return Const reference to the value. */
    const T& at(const std::string& key) const { return m_items[m_index.at(key)].second; }

    /** @brief Number of entries. @return Entry count. */
    std::size_t size() const { return m_items.size(); }
    /** @brief Whether the dict has no entries. @return True if empty. */
    bool empty() const { return m_items.empty(); }

    auto begin() { return m_items.begin(); }
    auto end() { return m_items.end(); }
    auto begin() const { return m_items.begin(); }
    auto end() const { return m_items.end(); }

private:
    std::vector<std::pair<std::string, T>> m_items;
    std::unordered_map<std::string, std::size_t> m_index;
};

/** @brief Trims ASCII whitespace from both ends. @param s String to trim. @return Trimmed string. */
std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
        return "";
    }
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/** @brief Trims any of the given characters from both ends, mirroring python's str.strip(chars). */
std::string stripChars(const std::string& s, const std::string& chars) {
    std::size_t a = s.find_first_not_of(chars);
    if (a == std::string::npos) {
        return "";
    }
    std::size_t b = s.find_last_not_of(chars);
    return s.substr(a, b - a + 1);
}

/** @brief Strips a single pair of surrounding double quotes, if present. */
std::string stripQuotes(const std::string& s) { return stripChars(s, "\""); }

/** @brief Splits a string on a delimiter char, keeping empty tokens, mirroring python str.split. */
std::vector<std::string> splitOn(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == delim) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

/** @brief Splits on comma with surrounding whitespace trimmed, mirroring re.split(r'\s*,\s*', ...). */
std::vector<std::string> splitArgs(const std::string& farg) {
    static const std::regex sep(R"(\s*,\s*)");
    std::vector<std::string> out(std::sregex_token_iterator(farg.begin(), farg.end(), sep, -1), std::sregex_token_iterator());
    return out;
}

/** @brief Mimics python re.match: true if re matches starting at position 0 of s. */
bool matchAtStart(const std::string& s, const std::regex& re, std::smatch& m) {
    return std::regex_search(s, m, re) && m.position(0) == 0;
}

/**
 * @brief Parses a non-negative-or-signed integer literal ("0x..." hex or decimal), mirroring
 *        python's eval(s) for the literal strings matched by this parser's int-literal regexes.
 *        Python 3 rejects a decimal literal with a leading zero followed by another digit (e.g.
 *        "0123") as a SyntaxError; unlike std::stoll(s, nullptr, 0), which would silently
 *        reinterpret such strings as octal, this throws instead.
 */
std::int64_t parsePyIntLiteral(const std::string& s) {
    const bool neg = !s.empty() && s.front() == '-';
    const std::string digits = neg ? s.substr(1) : s;

    std::int64_t value;
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        value = static_cast<std::int64_t>(std::stoll(digits, nullptr, 16));
    } else {
        if (digits.size() > 1 && digits.front() == '0' && digits.find_first_not_of('0') != std::string::npos) {
            throw std::runtime_error("invalid decimal literal (leading zeros not permitted): " + s);
        }
        value = static_cast<std::int64_t>(std::stoll(digits, nullptr, 10));
    }
    return neg ? -value : value;
}

/**
 * @brief Converts a header attribute's raw text into an int or a (possibly symbolic) string,
 *        mirroring CuAsmParser.__cvtValue.
 */
HeaderValue cvtValue(const std::string& s) {
    static const std::regex intval(R"(^(0x[a-fA-F0-9]+|[0-9]+))");
    if (std::regex_search(s, intval)) {
        return parsePyIntLiteral(s);
    }
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

/** @brief Looks up a symbolic ELF constant name (SHT_, PT_, ET_ family) used by this codebase's cuasm files. */
std::int64_t resolveNamedConstant(const std::string& name) {
    static const std::unordered_map<std::string, std::int64_t> table = {
        {"SHT_NULL", ELFIO::SHT_NULL},       {"SHT_PROGBITS", ELFIO::SHT_PROGBITS},
        {"SHT_SYMTAB", ELFIO::SHT_SYMTAB},   {"SHT_STRTAB", ELFIO::SHT_STRTAB},
        {"SHT_RELA", ELFIO::SHT_RELA},       {"SHT_NOBITS", ELFIO::SHT_NOBITS},
        {"SHT_REL", ELFIO::SHT_REL},         {"PT_LOAD", ELFIO::PT_LOAD},
        {"PT_PHDR", ELFIO::PT_PHDR},         {"ET_REL", ELFIO::ET_REL},
        {"ET_EXEC", ELFIO::ET_EXEC},         {"ET_DYN", ELFIO::ET_DYN},
    };
    auto it = table.find(name);
    if (it == table.end()) {
        throw std::runtime_error("Unknown named ELF constant: " + name);
    }
    return it->second;
}

/** @brief Resolves a HeaderValue to a concrete integer, handling numeric text and named ELF constants. */
std::int64_t resolveHeaderInt(const HeaderValue& v) {
    if (std::holds_alternative<std::int64_t>(v)) {
        return std::get<std::int64_t>(v);
    }
    const std::string& s = std::get<std::string>(v);
    static const std::regex numeric(R"(^-?(0x[a-fA-F0-9]+|[0-9]+)$)");
    if (std::regex_match(s, numeric)) {
        return parsePyIntLiteral(s);
    }
    return resolveNamedConstant(s);
}

/**
 * @brief Checks whether a HeaderValue is exactly the given symbolic name, mirroring Python's
 *        raw `header['type'] == 'SHT_NULL'` string comparison (never resolves numeric spellings
 *        of the same constant, e.g. `.__section_type 8` is not treated as SHT_NOBITS).
 */
bool isHeaderTypeNamed(const HeaderValue& v, const std::string& name) {
    return std::holds_alternative<std::string>(v) && std::get<std::string>(v) == name;
}

/** @brief Builds the string-keyed dict of NUL-terminated strings within a strtab/shstrtab section. */
std::map<std::uint32_t, std::string> buildStringDict(const std::string& bytelist) {
    std::map<std::uint32_t, std::string> sdict;
    std::size_t p = 0;
    while (true) {
        std::size_t pnext = bytelist.find('\0', p);
        if (pnext == std::string::npos) {
            break;
        }
        sdict[static_cast<std::uint32_t>(p)] = bytelist.substr(p, pnext - p);
        p = pnext + 1;
    }
    return sdict;
}

class CuAsmSection;

/**
 * @brief Explicitly-defined (.type/.size/.global/.weak/.other) symbol, mirroring CuAsmParser.CuAsmSymbol.
 **/
class CuAsmSymbol {
public:
    /** @brief Constructs a symbol with the given name. @param symbolName Symbol name. */
    explicit CuAsmSymbol(std::string symbolName) : name(std::move(symbolName)), entry(Config::defaultSymbol()) {}

    /** @brief Builds the (currently copied-from-cubin) symtab entry bytes for this symbol. @return The entry bytes. */
    std::string build() const {
        std::string bytes(sizeof(ELFIO::Elf64_Sym), '\0');
        std::memcpy(bytes.data(), &entry, sizeof(entry));
        return bytes;
    }

    /** @brief Formats a debug summary of this symbol. @return The summary string. */
    std::string toString() const {
        std::ostringstream o;
        o << "name=" << name << ", type=" << type.value_or("?") << ", value=" << (value ? std::to_string(*value) : "?")
          << ", size(" << (sizeval ? std::to_string(*sizeval) : "?") << ")=" << size.value_or("?");
        return o.str();
    }

    /** @brief Valid `.type` values recognized by nvdisasm cuasm output. @return Map of symbol type token to code. */
    static const std::map<std::string, int>& symbolTypes() {
        static const std::map<std::string, int> types = {
            {"@function", 0},
            {"@object", 1},
            {R"(@"STT_CUDA_TEXTURE")", 2},
            {R"(@"STT_CUDA_SURFACE")", 3},
        };
        return types;
    }

    /**
     * @brief Builds a name-keyed dict of (index, entry) from a raw .symtab section, mirroring
     *        CuAsmSymbol.buildSymbolDict.
     * @param strtab Offset-keyed strings dict decoded from .strtab.
     * @param symbytes Raw .symtab section bytes.
     * @return Name-keyed (insertion order = symtab order) dict of (index, entry).
     **/
    static OrderedDict<std::pair<int, ELFIO::Elf64_Sym>> buildSymbolDict(const std::map<std::uint32_t, std::string>& strtab,
                                                                          const std::string& symbytes) {
        OrderedDict<std::pair<int, ELFIO::Elf64_Sym>> symdict;
        const std::size_t symsize = sizeof(ELFIO::Elf64_Sym);
        int index = 0;
        for (std::size_t p = 0; p + symsize <= symbytes.size(); p += symsize) {
            ELFIO::Elf64_Sym sym{};
            std::memcpy(&sym, symbytes.data() + p, symsize);

            auto it = strtab.find(sym.st_name);
            if (it == strtab.end()) {
                throw std::runtime_error("Unknown symbol with name string index " + std::to_string(sym.st_name) + "!");
            }
            const std::string& name = it->second;
            if (symdict.contains(name)) {
                throw std::runtime_error("Duplicate symbol " + name + "!");
            }
            symdict[name] = {index, sym};
            ++index;
        }
        return symdict;
    }

    /**
     * @brief Overwrites the value/size fields of one .symtab entry in place, mirroring
     *        CuAsmSymbol.resetSymtabEntryValueSize.
     * @param symtabBytes Raw .symtab section bytes to patch.
     * @param baseOffset Byte offset of the entry within symtabBytes.
     * @param value New st_value.
     * @param size New st_size.
     **/
    static void resetSymtabEntryValueSize(std::string& symtabBytes, std::size_t baseOffset, std::uint64_t value, std::uint64_t size) {
        if (baseOffset + 24 > symtabBytes.size()) {
            return;
        }
        std::memcpy(symtabBytes.data() + baseOffset + 8, &value, 8);
        std::memcpy(symtabBytes.data() + baseOffset + 16, &size, 8);
    }

    std::string name;
    std::optional<std::string> type;
    std::optional<std::uint64_t> value;
    std::optional<std::string> size;
    std::optional<std::uint64_t> sizeval;
    std::optional<std::string> other;
    bool isGlobal = false;
    ELFIO::Elf64_Sym entry;
};

/**
 * @brief An ELF section being assembled, mirroring CuAsmParser.CuAsmSection. Only .section /
 *        supplementary `.__section_*` directives populate header; `type`/`flags` set from the
 *        `.section` directive itself are kept only for display, matching the original (the real
 *        section header is always driven by the `.__section_*` directives).
 **/
class CuAsmSection {
public:
    /**
     * @brief Constructs an ELF section.
     * @param sname Section name.
     * @param stype Cosmetic "type" token from `.section`; unused in header building.
     * @param sflags Cosmetic "flags" token from `.section`; unused in header building.
     **/
    CuAsmSection(std::string sname, std::string stype, std::string sflags)
        : name(std::move(sname)), type(std::move(stype)), flags{std::move(sflags)}, sectionHeader(Config::defaultSectionHeader()) {}

    /** @brief Merges header overrides into the concrete section-header struct, mirroring updateHeader. */
    void updateHeader() {
        for (const auto& [attr, val] : header) {
            std::int64_t iv;
            try {
                iv = resolveHeaderInt(val);
            } catch (...) {
                continue;
            }
            if (attr == "name") sectionHeader.sh_name = static_cast<ELFIO::Elf_Word>(iv);
            else if (attr == "type") sectionHeader.sh_type = static_cast<ELFIO::Elf_Word>(iv);
            else if (attr == "flags") sectionHeader.sh_flags = static_cast<ELFIO::Elf_Xword>(iv);
            else if (attr == "addr") sectionHeader.sh_addr = static_cast<ELFIO::Elf64_Addr>(iv);
            else if (attr == "offset") sectionHeader.sh_offset = static_cast<ELFIO::Elf64_Off>(iv);
            else if (attr == "size") sectionHeader.sh_size = static_cast<ELFIO::Elf_Xword>(iv);
            else if (attr == "link") sectionHeader.sh_link = static_cast<ELFIO::Elf_Word>(iv);
            else if (attr == "info") sectionHeader.sh_info = static_cast<ELFIO::Elf_Word>(iv);
            else if (attr == "entsize") sectionHeader.sh_entsize = static_cast<ELFIO::Elf_Xword>(iv);
            else if (attr == "addralign") sectionHeader.sh_addralign = static_cast<ELFIO::Elf_Xword>(iv);
        }

        if (header.count("type") && isHeaderTypeNamed(header.at("type"), "SHT_NULL")) {
            sectionHeader.sh_offset = 0;
        } else {
            sectionHeader.sh_offset = offset;
        }
        sectionHeader.sh_size = getDataSize();
    }

    /** @brief Gets the concrete section-header struct. @return The struct. */
    const ELFIO::Elf64_Shdr& getHeaderStruct() const { return sectionHeader; }

    /**
     * @brief Updates register/barrier counts from `.sectioninfo`/`.sectionflags`, mirroring
     *        updateResourceInfo.
     **/
    void updateResourceInfo() {
        static const std::regex pRegnum(R"re(@"SHI_REGISTERS=(\d+)")re");
        static const std::regex pBarnum(R"re(@"SHF_BARRIERS=(\d+)")re");

        std::optional<int> regnum;
        int barnum = 0;

        for (const auto& infoStr : info) {
            std::smatch m;
            if (std::regex_match(infoStr, m, pRegnum)) {
                regnum = std::stoi(m[1].str());
            }
        }
        for (const auto& flagStr : flags) {
            std::smatch m;
            if (std::regex_match(flagStr, m, pBarnum)) {
                barnum = std::stoi(m[1].str());
            }
        }

        if (!regnum) {
            throw std::runtime_error("Unknown register number for section " + name + "!");
        }
        if (*regnum > 255 || *regnum < 0) {
            throw std::runtime_error("Invalid register number " + std::to_string(*regnum) + " for section " + name + "!");
        }

        std::int64_t rinfo = header.count("info") ? resolveHeaderInt(header.at("info")) : 0;
        header["info"] = (rinfo & 0x00ffffffLL) + (static_cast<std::int64_t>(*regnum) << 24);
        extra["regnum"] = *regnum;

        if (barnum > 15) {
            throw std::runtime_error("Invalid barrier number " + std::to_string(barnum) + " for section " + name + "!");
        }
        std::int64_t rflag = header.count("flags") ? resolveHeaderInt(header.at("flags")) : 0;
        header["flags"] = (rflag & 0xff0fffffLL) + (static_cast<std::int64_t>(barnum) << 20);
        extra["barnum"] = barnum;
    }

    /** @brief Builds the section-header bytes, updating the header first. @return The header bytes. */
    std::string buildHeader() {
        updateHeader();
        std::string bytes(sizeof(ELFIO::Elf64_Shdr), '\0');
        std::memcpy(bytes.data(), &sectionHeader, sizeof(sectionHeader));
        return bytes;
    }

    /** @brief Appends bytes at the current position, growing the buffer as needed. @param bs Bytes to write. */
    void emitBytes(const std::string& bs) {
        if (m_pos + bs.size() > m_data.size()) {
            m_data.resize(m_pos + bs.size(), '\0');
        }
        if (!bs.empty()) {
            std::memcpy(m_data.data() + m_pos, bs.data(), bs.size());
        }
        m_pos += bs.size();
    }

    /**
     * @brief Overwrites bytes at an absolute offset without moving the current position,
     *        mirroring updateForFixup.
     * @param fixupOffset Absolute offset within the section.
     * @param bs Bytes to write.
     **/
    void updateForFixup(std::uint64_t fixupOffset, const std::string& bs) {
        if (fixupOffset + bs.size() > getDataSize()) {
            throw std::runtime_error("Fixup out of boundary!");
        }
        std::uint64_t opos = tell();
        seek(static_cast<std::int64_t>(fixupOffset), 0);
        emitBytes(bs);
        seek(static_cast<std::int64_t>(opos), 0);
    }

    /** @brief Pads to the given alignment, mirroring emitAlign. @param align Power-of-2 alignment. */
    void emitAlign(int align) {
        std::uint64_t pos = tell();
        if (pos == 0) {
            addralign = static_cast<std::uint64_t>(align);
            header["addralign"] = static_cast<std::int64_t>(align);
        } else {
            auto [ppos, padsz] = alignTo(pos, static_cast<std::uint64_t>(align));
            (void)padsz;
            if (ppos > pos) {
                emitBytes(std::string(ppos - pos, '\0'));
            }
        }
    }

    /** @brief Appends bytes at the end without disturbing the current position, mirroring emitPadding. */
    void emitPadding(const std::string& bs) {
        std::uint64_t pos = tell();
        seek(0, 2);
        emitBytes(bs);
        seek(static_cast<std::int64_t>(pos), 0);
    }

    /** @brief Seeks within the section buffer. @param pos Offset. @param whence 0=set,1=cur,2=end. */
    void seek(std::int64_t pos, int whence = 0) {
        if (whence == 0) {
            m_pos = static_cast<std::uint64_t>(pos);
        } else if (whence == 1) {
            m_pos = static_cast<std::uint64_t>(static_cast<std::int64_t>(m_pos) + pos);
        } else {
            m_pos = m_data.size() + static_cast<std::uint64_t>(pos);
        }
    }

    /** @brief Gets the current buffer position. @return The position. */
    std::uint64_t tell() const { return m_pos; }

    /** @brief Gets the full section data buffer. @return The buffer. */
    const std::string& getData() const { return m_data; }

    /** @brief Writes this section's data (and padding) to a stream, mirroring writePaddedData. */
    void writePaddedData(std::ostream& os) const {
        if (header.count("type") && isHeaderTypeNamed(header.at("type"), "SHT_NOBITS")) {
            return;
        }
        os.write(m_data.data(), static_cast<std::streamsize>(m_data.size()));
        os.write(padbytes.data(), static_cast<std::streamsize>(padbytes.size()));
    }

    /** @brief Replaces the section's data buffer, resetting position to 0. @param bs New data. */
    void setData(std::string bs) {
        size = bs.size();
        m_data = std::move(bs);
        m_pos = 0;
    }

    /** @brief Gets the in-memory data size. @return Size in bytes. */
    std::uint64_t getDataSize() const { return m_data.size(); }

    /** @brief Gets the data size plus separate padding. @return Padded size in bytes. */
    std::uint64_t getPaddedDataSize() const { return getDataSize() + padsize; }

    /** @brief Gets the register count recorded by updateResourceInfo. @return Register count. */
    int getRegNum() const { return extra.at("regnum"); }

    std::string name;
    std::string type;
    std::vector<std::string> flags;
    std::vector<std::string> info;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t addralign = 0;
    std::uint64_t entsize = 0;

    std::map<std::string, HeaderValue> header;
    std::map<std::string, int> extra;

    std::uint64_t padsize = 0;
    std::string padbytes;

private:
    ELFIO::Elf64_Shdr sectionHeader;
    std::string m_data;
    std::uint64_t m_pos = 0;
};

/**
 * @brief A label defined by "label:", mirroring CuAsmParser.CuAsmLabel.
 **/
class CuAsmLabel {
public:
    /**
     * @brief Constructs and logs a new label.
     * @param labelName Label name.
     * @param labelSection Section the label is defined in (non-owning).
     * @param labelOffset Byte offset within the section.
     * @param labelLineNo Source line number.
     **/
    CuAsmLabel(std::string labelName, CuAsmSection* labelSection, std::uint64_t labelOffset, int labelLineNo)
        : name(std::move(labelName)), section(labelSection), offset(labelOffset), lineNo(labelLineNo) {
        std::ostringstream o;
        o << "Line " << std::setw(6) << lineNo << ": New Label \"" << name << "\" at section \"" << section->name
          << "\":0x" << std::hex << offset;
        CuAsmLogger::logSubroutine(o.str());
    }

    /** @brief Formats a debug summary of this label. @return The summary string. */
    std::string toString() const {
        std::ostringstream o;
        o << "Label @Line " << std::setw(4) << lineNo << " in section " << section->name << " : 0x" << std::hex << offset
          << std::dec << "(" << offset << ")  " << name;
        return o.str();
    }

    std::string name;
    CuAsmSection* section;
    std::uint64_t offset;
    int lineNo;
};

/**
 * @brief An undetermined value evaluated after the first parsing scan, mirroring
 *        CuAsmParser.CuAsmFixup.
 **/
class CuAsmFixup {
public:
    /**
     * @brief Constructs and logs a new fixup.
     * @param fixupSection Section the fixup belongs to (non-owning).
     * @param fixupOffset Byte offset within the section.
     * @param fixupExpr Fixup expression text.
     * @param fixupDtype Data type ("byte"/"short"/"word"/"dword").
     * @param fixupLineNo Source line number.
     **/
    CuAsmFixup(CuAsmSection* fixupSection, std::uint64_t fixupOffset, std::string fixupExpr, std::string fixupDtype, int fixupLineNo)
        : section(fixupSection), offset(fixupOffset), lineNo(fixupLineNo), dtype(std::move(fixupDtype)), expr(std::move(fixupExpr)) {
        std::ostringstream o;
        o << "Line " << std::setw(6) << lineNo << ": New Fixup \"" << expr << "\" at section \"" << section->name << "\":0x"
          << std::hex << offset;
        CuAsmLogger::logSubroutine(o.str());
    }

    /** @brief Formats a debug summary of this fixup. @return The summary string. */
    std::string toString() const {
        std::ostringstream o;
        o << "section=" << section->name << ", offset=" << offset << ", lineno=" << lineNo << ", dtype=" << dtype
          << ", expr=" << expr << ", value=" << (value ? std::to_string(*value) : "None");
        return o.str();
    }

    CuAsmSection* section;
    std::uint64_t offset;
    int lineNo;
    std::string dtype;
    std::string expr;
    std::optional<std::int64_t> value;
};

/**
 * @brief An ELF program-header segment being assembled, mirroring CuAsmParser.CuAsmSegment.
 **/
class CuAsmSegment {
public:
    /**
     * @brief Constructs a segment.
     * @param pType Segment type token, e.g. "PT_LOAD"/"PT_PHDR".
     * @param pFlags Segment flags text (numeric).
     **/
    CuAsmSegment(std::string pType, std::string pFlags) : segmentHeader(Config::defaultSegmentHeader()) {
        header["type"] = HeaderValue(std::move(pType));
        header["flags"] = HeaderValue(std::move(pFlags));
    }

    /** @brief Merges header overrides into the concrete segment-header struct. */
    void updateHeader() {
        for (const auto& [attr, val] : header) {
            std::int64_t iv;
            try {
                iv = resolveHeaderInt(val);
            } catch (...) {
                continue;
            }
            if (attr == "type") segmentHeader.p_type = static_cast<ELFIO::Elf_Word>(iv);
            else if (attr == "flags") segmentHeader.p_flags = static_cast<ELFIO::Elf_Word>(iv);
            else if (attr == "offset") segmentHeader.p_offset = static_cast<ELFIO::Elf64_Off>(iv);
            else if (attr == "vaddr") segmentHeader.p_vaddr = static_cast<ELFIO::Elf64_Addr>(iv);
            else if (attr == "paddr") segmentHeader.p_paddr = static_cast<ELFIO::Elf64_Addr>(iv);
            else if (attr == "filesz") segmentHeader.p_filesz = static_cast<ELFIO::Elf_Xword>(iv);
            else if (attr == "memsz") segmentHeader.p_memsz = static_cast<ELFIO::Elf_Xword>(iv);
            else if (attr == "align") segmentHeader.p_align = static_cast<ELFIO::Elf_Xword>(iv);
        }
    }

    /** @brief Gets the concrete segment-header struct. @return The struct. */
    const ELFIO::Elf64_Phdr& getHeaderStruct() const { return segmentHeader; }

    /** @brief Serializes the (already up to date) segment header to bytes. @return The header bytes. */
    std::string build() const {
        std::string bytes(sizeof(ELFIO::Elf64_Phdr), '\0');
        std::memcpy(bytes.data(), &segmentHeader, sizeof(segmentHeader));
        return bytes;
    }

    std::map<std::string, HeaderValue> header;

private:
    ELFIO::Elf64_Phdr segmentHeader;
};

/**
 * @brief A REL/RELA relocation entry, mirroring CuAsmParser.CuAsmRelocation.
 **/
class CuAsmRelocation {
public:
    /**
     * @brief Constructs and logs a new relocation.
     * @param relSection Section the relocation applies to (non-owning).
     * @param relOffset Byte offset within relSection.
     * @param relSymNameIn Referenced symbol name.
     * @param relSymIdIn Referenced symbol's .symtab index, if known.
     * @param relTypeIn Relocation type name, e.g. "R_CUDA_32".
     * @param relAddIn Addend; present means a RELA entry, absent means REL.
     **/
    CuAsmRelocation(CuAsmSection* relSection, std::uint64_t relOffset, std::string relSymNameIn, std::optional<int> relSymIdIn,
                     std::string relTypeIn, std::optional<std::int64_t> relAddIn = std::nullopt)
        : section(relSection), offset(relOffset), relSymName(std::move(relSymNameIn)), relSymId(relSymIdIn),
          relType(std::move(relTypeIn)), relAdd(relAddIn) {
        CuAsmLogger::logSubroutine("New Relocation \"" + relSymName + "\" at section \"" + section->name + "\"");
    }

    /** @brief Whether this is a RELA (has addend) entry. @return True if RELA. */
    bool isRELA() const { return relAdd.has_value(); }

    /** @brief Builds the binary REL/RELA entry bytes, mirroring buildEntry. @return The entry bytes. */
    std::string buildEntry() const {
        static const std::map<std::string, int> relTypes = {
            {"R_CUDA_32", 1},          {"R_CUDA_64", 2},
            {"R_CUDA_G64", 4},         {"R_CUDA_TEX_HEADER_INDEX", 6},
            {"R_CUDA_SURF_HEADER_INDEX", 52}, {"R_CUDA_ABS32_20", 42},
            {"R_CUDA_ABS32_LO_20", 43}, {"R_CUDA_ABS32_HI_20", 44},
            {"R_CUDA_ABS32_LO_32", 56}, {"R_CUDA_ABS32_HI_32", 57},
            {"R_CUDA_ABS47_34", 58},
        };
        std::uint64_t typeCode = static_cast<std::uint64_t>(relTypes.at(relType));
        std::uint64_t infoSym = static_cast<std::uint64_t>(relSymId.value_or(0));

        if (isRELA()) {
            ELFIO::Elf64_Rela rela = Config::defaultRela();
            rela.r_offset = offset;
            rela.r_info = (infoSym << 32) + typeCode;
            rela.r_addend = *relAdd;
            std::string bytes(sizeof(rela), '\0');
            std::memcpy(bytes.data(), &rela, sizeof(rela));
            return bytes;
        }

        ELFIO::Elf64_Rel rel = Config::defaultRel();
        rel.r_offset = offset;
        rel.r_info = (infoSym << 32) + typeCode;
        std::string bytes(sizeof(rel), '\0');
        std::memcpy(bytes.data(), &rel, sizeof(rel));
        return bytes;
    }

    /** @brief Formats a debug summary of this relocation. @return The summary string. */
    std::string toString() const {
        std::ostringstream o;
        o << "@section " << section->name << ": offset=" << offset << ", relsym=" << relSymId.value_or(-1) << "(" << relSymName
          << "), reltype=" << relType << ", reladd=" << (relAdd ? std::to_string(*relAdd) : "None");
        return o.str();
    }

    CuAsmSection* section;
    std::uint64_t offset;
    std::string relSymName;
    std::optional<int> relSymId;
    std::string relType;
    std::optional<std::int64_t> relAdd;
};

/**
 * @brief The ELF file header being assembled, mirroring the subset of CuAsmParser.CuAsmFile
 *        actually used by CuAsmParser (the python class's internal BytesIO buffer/section list is
 *        dead code from CuAsmParser's point of view and is not ported).
 **/
class CuAsmFile {
public:
    /** @brief Constructs a file header pre-seeded with the default cubin ELF header. */
    CuAsmFile() : fileHeaderStruct(Config::defaultCubinFileHeader()) {}

    /** @brief Builds the ELF file header bytes from fileHeader, mirroring buildFileHeader. @return The header bytes. */
    std::string buildFileHeader() {
        fileHeaderStruct.e_ident[ELFIO::EI_OSABI] = static_cast<unsigned char>(resolveHeaderInt(fileHeader.at("ident_osabi")));
        fileHeaderStruct.e_ident[ELFIO::EI_ABIVERSION] = static_cast<unsigned char>(resolveHeaderInt(fileHeader.at("ident_abiversion")));
        fileHeaderStruct.e_type = static_cast<ELFIO::Elf_Half>(resolveHeaderInt(fileHeader.at("type")));
        fileHeaderStruct.e_machine = static_cast<ELFIO::Elf_Half>(resolveHeaderInt(fileHeader.at("machine")));
        fileHeaderStruct.e_version = static_cast<ELFIO::Elf_Word>(resolveHeaderInt(fileHeader.at("version")));
        fileHeaderStruct.e_entry = static_cast<ELFIO::Elf64_Addr>(resolveHeaderInt(fileHeader.at("entry")));
        fileHeaderStruct.e_phoff = static_cast<ELFIO::Elf64_Off>(resolveHeaderInt(fileHeader.at("phoff")));
        fileHeaderStruct.e_shoff = static_cast<ELFIO::Elf64_Off>(resolveHeaderInt(fileHeader.at("shoff")));
        fileHeaderStruct.e_flags = static_cast<ELFIO::Elf_Word>(resolveHeaderInt(fileHeader.at("flags")));
        fileHeaderStruct.e_ehsize = static_cast<ELFIO::Elf_Half>(resolveHeaderInt(fileHeader.at("ehsize")));
        fileHeaderStruct.e_phentsize = static_cast<ELFIO::Elf_Half>(resolveHeaderInt(fileHeader.at("phentsize")));
        fileHeaderStruct.e_phnum = static_cast<ELFIO::Elf_Half>(resolveHeaderInt(fileHeader.at("phnum")));
        fileHeaderStruct.e_shentsize = static_cast<ELFIO::Elf_Half>(resolveHeaderInt(fileHeader.at("shentsize")));
        fileHeaderStruct.e_shnum = static_cast<ELFIO::Elf_Half>(resolveHeaderInt(fileHeader.at("shnum")));
        fileHeaderStruct.e_shstrndx = static_cast<ELFIO::Elf_Half>(resolveHeaderInt(fileHeader.at("shstrndx")));

        std::string bytes(sizeof(ELFIO::Elf64_Ehdr), '\0');
        std::memcpy(bytes.data(), &fileHeaderStruct, sizeof(fileHeaderStruct));
        return bytes;
    }

    /** @brief Gets the concrete ELF file-header struct. @return The struct. */
    const ELFIO::Elf64_Ehdr& getFileHeaderStruct() const { return fileHeaderStruct; }

    std::map<std::string, HeaderValue> fileHeader;
    std::optional<std::string> headerflags;
    std::optional<std::string> elftype;

private:
    ELFIO::Elf64_Ehdr fileHeaderStruct;
};

/** @brief Result of evaluating an expression, mirroring the (value, (a, op, b)) tuple __evalExpr returns. */
struct ExprResult {
    std::int64_t value = 0;
    std::variant<std::int64_t, std::string> aReal;
    std::optional<char> op;
    std::optional<std::int64_t> bval;
};

/** @brief Dumps an Elf64_Ehdr as a flat "field=value" line for debug comparison. */
std::string dumpEhdr(const ELFIO::Elf64_Ehdr& h) {
    std::ostringstream o;
    o << "e_type=" << h.e_type << " e_machine=" << h.e_machine << " e_version=" << h.e_version << " e_entry=0x" << std::hex
      << h.e_entry << " e_phoff=0x" << h.e_phoff << " e_shoff=0x" << h.e_shoff << " e_flags=0x" << h.e_flags << std::dec
      << " e_ehsize=" << h.e_ehsize << " e_phentsize=" << h.e_phentsize << " e_phnum=" << h.e_phnum
      << " e_shentsize=" << h.e_shentsize << " e_shnum=" << h.e_shnum << " e_shstrndx=" << h.e_shstrndx;
    return o.str();
}

/** @brief Dumps an Elf64_Shdr as a flat "field=value" line for debug comparison. */
std::string dumpShdr(const ELFIO::Elf64_Shdr& h) {
    std::ostringstream o;
    o << "sh_name=" << h.sh_name << " sh_type=" << h.sh_type << " sh_flags=0x" << std::hex << h.sh_flags << " sh_addr=0x"
      << h.sh_addr << " sh_offset=0x" << h.sh_offset << std::dec << " sh_size=" << h.sh_size << " sh_link=" << h.sh_link
      << " sh_info=" << h.sh_info << " sh_addralign=" << h.sh_addralign << " sh_entsize=" << h.sh_entsize;
    return o.str();
}

/** @brief Dumps an Elf64_Phdr as a flat "field=value" line for debug comparison. */
std::string dumpPhdr(const ELFIO::Elf64_Phdr& h) {
    std::ostringstream o;
    o << "p_type=" << h.p_type << " p_flags=" << h.p_flags << " p_offset=0x" << std::hex << h.p_offset << " p_vaddr=0x"
      << h.p_vaddr << " p_paddr=0x" << h.p_paddr << std::dec << " p_filesz=" << h.p_filesz << " p_memsz=" << h.p_memsz
      << " p_align=" << h.p_align;
    return o.str();
}

} // namespace

/**
 * @brief Private implementation of CuAsmParser, holding all per-parse state and the internal
 *        helper types the original python module defines at file scope.
 **/
struct CuAsmParser::Impl {
    /** @brief Constructs the parser, wiring up the directive dispatch table. */
    Impl() { buildDirDict(); }

    // ---- construction / reset ----

    /** @brief Registers all `.directive` handlers, mirroring CuAsmParser.__init__'s __dirDict. */
    void buildDirDict();

    /** @brief Resets all per-parse state, mirroring CuAsmParser.reset. */
    void reset();

    // ---- public entry points (called from CuAsmParser's methods) ----

    /** @brief Sets the instruction-assembler repository from a saved file, mirroring setInsAsmRepos. */
    void setInsAsmRepos(const std::string& fname, const std::string& arch) {
        mCuInsAsmRepos = std::make_unique<CuInsAssemblerRepos>(fname, arch);
    }

    /** @brief Parses a .cuasm file end to end, mirroring CuAsmParser.parse. */
    void parse(const std::string& fname);

    /** @brief Assembles and writes the parsed source as a .cubin file, mirroring saveAsCubin. */
    void saveAsCubin(const std::string& path);

    /** @brief Writes side-by-side debug dumps comparing against a reference cubin, mirroring saveCubinCmp. */
    void saveCubinCmp(const std::string& cubinName, const std::string& savPrefix);

    void dispFixupList() const;
    void dispRelocationList() const;
    void dispSectionList() const;
    void dispSymbolDict() const;
    void dispSymtabDict() const;
    void dispLabelDict() const;
    void dispSegmentHeader() const;
    void dispFileHeader() const;
    void dispTables() const;

    // ---- parsing procedures ----

    void preScan();
    void gatherTextSectionSizeLabel();
    void parseKernels();
    void buildInternalTables();
    void parseKernelText(CuAsmSection& section, int lineStart, int lineEnd);
    void buildRelocationSections();
    void evalFixups();
    void updateSymtab();
    void layoutSections();

    // ---- directives ----

    void dirHeaderflags(const std::vector<std::string>& args);
    void dirElftype(const std::vector<std::string>& args);
    void dirSection(const std::vector<std::string>& args);
    void dirSectionflags(const std::vector<std::string>& args);
    void dirSectionentsize(const std::vector<std::string>& args);
    void dirSectioninfo(const std::vector<std::string>& args);
    void dirByte(const std::vector<std::string>& args);
    void dirDword(const std::vector<std::string>& args);
    void dirAlign(const std::vector<std::string>& args);
    void dirShort(const std::vector<std::string>& args);
    void dirWord(const std::vector<std::string>& args);
    void dirType(const std::vector<std::string>& args);
    void dirSize(const std::vector<std::string>& args);
    void dirGlobal(const std::vector<std::string>& args);
    void dirWeak(const std::vector<std::string>& args);
    void dirZero(const std::vector<std::string>& args);
    void dirOther(const std::vector<std::string>& args);
    void dirElfheader(const std::string& attrname, const std::vector<std::string>& args);
    void dirSectionheader(const std::string& attrname, const std::vector<std::string>& args);
    void dirSegment(const std::vector<std::string>& args);
    void dirSegmentheader(const std::string& attrname, const std::vector<std::string>& args);

    // ---- subroutines ----

    void doAssert(bool flag, const std::string& msg = "");
    void assertArgc(const std::string& cmd, const std::vector<std::string>& args, std::size_t argc, bool allowMore);
    std::uint64_t tellLocal();
    std::pair<std::int64_t, bool> evalVar(const std::string& var);
    ExprResult evalExpr(const std::string& expr);
    std::optional<int> getSymbolIdx(const std::string& symname);
    std::string evalInstructionFixup(CuAsmSection& section, std::uint64_t offset, const std::string& s);
    void updateSectionForFixup(CuAsmFixup& fixup);
    void emitBytes(const std::string& bs);

    enum class LineType { Blank, Label, Directive, Code, Unknown };
    LineType getLineType(const std::string& line);
    void emitTypedBytes(const std::string& dtype, const std::vector<std::string>& args);
    std::string genSectionPaddingBytes(CuAsmSection& sec, std::uint64_t padSize);
    std::pair<std::uint64_t, std::uint64_t> updateSectionPadding(CuAsmSection* sec, std::uint64_t fileOffset, std::uint64_t memOffset,
                                                                   std::uint64_t align);
    std::string checkNVInfoOffsetLabels(CuAsmSection* section, const std::string& labelname, std::uint64_t offset);

    // ---- state ----

    std::unique_ptr<CuInsAssemblerRepos> mCuInsAsmRepos;
    std::map<std::string, std::function<void(const std::vector<std::string>&)>> mDirDict;

    int mLineNo = 0;
    bool mInTextSection = false;

    CuAsmSection* mCurrSection = nullptr;
    CuAsmSegment* mCurrSegment = nullptr;
    CuAsmFile mCuAsmFile;

    OrderedDict<std::unique_ptr<CuAsmSection>> mSectionDict;
    OrderedDict<std::unique_ptr<CuAsmSymbol>> mSymbolDict;
    std::vector<std::unique_ptr<CuAsmSegment>> mSegmentList;
    std::vector<std::unique_ptr<CuAsmFixup>> mFixupList;

    OrderedDict<std::unique_ptr<CuAsmLabel>> mLabelDict;
    std::map<std::string, CuAsmLabel*> mSecSizeLabel;
    std::vector<std::unique_ptr<CuAsmRelocation>> mRelList;

    std::map<std::string, std::map<std::string, std::vector<std::uint32_t>>> mNVInfoOffsetLabels;
    int mInsIndex = 0;
    std::unique_ptr<CuSMVersion> mArch;

    std::uint64_t mPadSizeBeforeSecHeader = 0;

    std::string mFilename;
    std::vector<std::string> mLines;

    std::map<std::uint32_t, std::string> mShstrtabDict;
    std::map<std::uint32_t, std::string> mStrtabDict;
    OrderedDict<std::pair<int, ELFIO::Elf64_Sym>> mSymtabDict;

    static const std::regex mLabelPattern;
    static const std::regex mDirectivePattern;
};

const std::regex CuAsmParser::Impl::mLabelPattern(R"(([a-zA-Z0-9._$@#]+?)\s*:\s*(.*))");
const std::regex CuAsmParser::Impl::mDirectivePattern(R"((\.[a-zA-Z0-9_]+)\s*(.*))");

void CuAsmParser::Impl::buildDirDict() {
    mDirDict[".headerflags"] = [this](const std::vector<std::string>& a) { dirHeaderflags(a); };
    mDirDict[".elftype"] = [this](const std::vector<std::string>& a) { dirElftype(a); };
    mDirDict[".section"] = [this](const std::vector<std::string>& a) { dirSection(a); };
    mDirDict[".sectioninfo"] = [this](const std::vector<std::string>& a) { dirSectioninfo(a); };
    mDirDict[".sectionflags"] = [this](const std::vector<std::string>& a) { dirSectionflags(a); };
    mDirDict[".sectionentsize"] = [this](const std::vector<std::string>& a) { dirSectionentsize(a); };
    mDirDict[".align"] = [this](const std::vector<std::string>& a) { dirAlign(a); };
    mDirDict[".byte"] = [this](const std::vector<std::string>& a) { dirByte(a); };
    mDirDict[".short"] = [this](const std::vector<std::string>& a) { dirShort(a); };
    mDirDict[".word"] = [this](const std::vector<std::string>& a) { dirWord(a); };
    mDirDict[".dword"] = [this](const std::vector<std::string>& a) { dirDword(a); };
    mDirDict[".type"] = [this](const std::vector<std::string>& a) { dirType(a); };
    mDirDict[".size"] = [this](const std::vector<std::string>& a) { dirSize(a); };
    mDirDict[".global"] = [this](const std::vector<std::string>& a) { dirGlobal(a); };
    mDirDict[".weak"] = [this](const std::vector<std::string>& a) { dirWeak(a); };
    mDirDict[".zero"] = [this](const std::vector<std::string>& a) { dirZero(a); };
    mDirDict[".other"] = [this](const std::vector<std::string>& a) { dirOther(a); };

    mDirDict[".__elf_ident_osabi"] = [this](const std::vector<std::string>& a) { dirElfheader("ident_osabi", a); };
    mDirDict[".__elf_ident_abiversion"] = [this](const std::vector<std::string>& a) { dirElfheader("ident_abiversion", a); };
    mDirDict[".__elf_type"] = [this](const std::vector<std::string>& a) { dirElfheader("type", a); };
    mDirDict[".__elf_machine"] = [this](const std::vector<std::string>& a) { dirElfheader("machine", a); };
    mDirDict[".__elf_version"] = [this](const std::vector<std::string>& a) { dirElfheader("version", a); };
    mDirDict[".__elf_entry"] = [this](const std::vector<std::string>& a) { dirElfheader("entry", a); };
    mDirDict[".__elf_phoff"] = [this](const std::vector<std::string>& a) { dirElfheader("phoff", a); };
    mDirDict[".__elf_shoff"] = [this](const std::vector<std::string>& a) { dirElfheader("shoff", a); };
    mDirDict[".__elf_flags"] = [this](const std::vector<std::string>& a) { dirElfheader("flags", a); };
    mDirDict[".__elf_ehsize"] = [this](const std::vector<std::string>& a) { dirElfheader("ehsize", a); };
    mDirDict[".__elf_phentsize"] = [this](const std::vector<std::string>& a) { dirElfheader("phentsize", a); };
    mDirDict[".__elf_phnum"] = [this](const std::vector<std::string>& a) { dirElfheader("phnum", a); };
    mDirDict[".__elf_shentsize"] = [this](const std::vector<std::string>& a) { dirElfheader("shentsize", a); };
    mDirDict[".__elf_shnum"] = [this](const std::vector<std::string>& a) { dirElfheader("shnum", a); };
    mDirDict[".__elf_shstrndx"] = [this](const std::vector<std::string>& a) { dirElfheader("shstrndx", a); };

    mDirDict[".__section_name"] = [this](const std::vector<std::string>& a) { dirSectionheader("name", a); };
    mDirDict[".__section_type"] = [this](const std::vector<std::string>& a) { dirSectionheader("type", a); };
    mDirDict[".__section_flags"] = [this](const std::vector<std::string>& a) { dirSectionheader("flags", a); };
    mDirDict[".__section_addr"] = [this](const std::vector<std::string>& a) { dirSectionheader("addr", a); };
    mDirDict[".__section_offset"] = [this](const std::vector<std::string>& a) { dirSectionheader("offset", a); };
    mDirDict[".__section_size"] = [this](const std::vector<std::string>& a) { dirSectionheader("size", a); };
    mDirDict[".__section_link"] = [this](const std::vector<std::string>& a) { dirSectionheader("link", a); };
    mDirDict[".__section_info"] = [this](const std::vector<std::string>& a) { dirSectionheader("info", a); };
    mDirDict[".__section_entsize"] = [this](const std::vector<std::string>& a) { dirSectionheader("entsize", a); };

    mDirDict[".__segment"] = [this](const std::vector<std::string>& a) { dirSegment(a); };
    mDirDict[".__segment_offset"] = [this](const std::vector<std::string>& a) { dirSegmentheader("offset", a); };
    mDirDict[".__segment_vaddr"] = [this](const std::vector<std::string>& a) { dirSegmentheader("vaddr", a); };
    mDirDict[".__segment_paddr"] = [this](const std::vector<std::string>& a) { dirSegmentheader("paddr", a); };
    mDirDict[".__segment_filesz"] = [this](const std::vector<std::string>& a) { dirSegmentheader("filesz", a); };
    mDirDict[".__segment_memsz"] = [this](const std::vector<std::string>& a) { dirSegmentheader("memsz", a); };
    mDirDict[".__segment_align"] = [this](const std::vector<std::string>& a) { dirSegmentheader("align", a); };
    mDirDict[".__segment_startsection"] = [this](const std::vector<std::string>& a) { dirSegmentheader("startsection", a); };
    mDirDict[".__segment_endsection"] = [this](const std::vector<std::string>& a) { dirSegmentheader("endsection", a); };
}

void CuAsmParser::Impl::reset() {
    mLineNo = 0;
    mInTextSection = false;

    mCurrSection = nullptr;
    mCurrSegment = nullptr;
    mCuAsmFile = CuAsmFile();

    mSectionDict = OrderedDict<std::unique_ptr<CuAsmSection>>();
    mSymbolDict = OrderedDict<std::unique_ptr<CuAsmSymbol>>();
    mSegmentList.clear();
    mFixupList.clear();

    mLabelDict = OrderedDict<std::unique_ptr<CuAsmLabel>>();
    mSecSizeLabel.clear();
    mRelList.clear();

    mNVInfoOffsetLabels.clear();
    mInsIndex = 0;
    mArch.reset();

    mPadSizeBeforeSecHeader = 0;
}

void CuAsmParser::Impl::preScan() {
    for (const auto& rawLine : mLines) {
        std::string nline = trim(CuAsmParser::stripComments(rawLine));
        ++mLineNo;

        if (nline.empty()) {
            continue;
        }

        LineType ltype = getLineType(nline);
        if (ltype == LineType::Unknown) {
            doAssert(false, "Unreconized line contents:\n     " + rawLine);
        } else if (ltype == LineType::Label) {
            std::smatch m;
            matchAtStart(nline, mLabelPattern, m);
            std::string rlabel = m[1].str();
            std::uint64_t pos = tellLocal();

            std::string label = checkNVInfoOffsetLabels(mCurrSection, rlabel, pos);

            if (!mLabelDict.contains(label)) {
                mLabelDict[label] = std::make_unique<CuAsmLabel>(label, mCurrSection, pos, mLineNo);
            } else {
                CuAsmLabel& v = *mLabelDict.at(label);
                doAssert(false, "Redefinition of label " + v.name + "! First occurrence in Line" + std::to_string(v.lineNo) + "!");
            }
        } else if (ltype == LineType::Directive) {
            std::smatch m;
            matchAtStart(nline, mDirectivePattern, m);
            std::string cmd = m[1].str();

            auto it = mDirDict.find(cmd);
            doAssert(it != mDirDict.end(), "Unknown directive " + cmd + "!!!");

            std::string farg = trim(m[2].str());
            std::vector<std::string> args;
            if (!farg.empty()) {
                args = splitArgs(farg);
            }
            it->second(args);
        } else if (ltype == LineType::Code) {
            std::uint64_t pos = mArch->getInsOffsetFromIndex(mInsIndex);
            mCurrSection->seek(static_cast<std::int64_t>(pos), 0);
            emitBytes(std::string(static_cast<std::size_t>(mArch->getInstructionLength()), '\0'));
            ++mInsIndex;
        }
    }
}

void CuAsmParser::Impl::gatherTextSectionSizeLabel() {
    mSecSizeLabel.clear();
    for (auto& [label, labelPtr] : mLabelDict) {
        const std::string& secname = labelPtr->section->name;
        if (secname.rfind(".text", 0) != 0) {
            continue;
        }
        if (labelPtr->offset == mSectionDict.at(secname)->getDataSize()) {
            mSecSizeLabel[secname] = labelPtr.get();
        }
    }
}

void CuAsmParser::Impl::buildInternalTables() {
    mShstrtabDict = buildStringDict(mSectionDict.at(".shstrtab")->getData());
    mStrtabDict = buildStringDict(mSectionDict.at(".strtab")->getData());
    mSymtabDict = CuAsmSymbol::buildSymbolDict(mStrtabDict, mSectionDict.at(".symtab")->getData());
}

void CuAsmParser::Impl::parseKernels() {
    std::map<std::string, std::pair<int, int>> sectionMarkers = splitAsmSection(mLines);
    std::map<int, int> regnumdict;

    for (const auto& [secname, range] : sectionMarkers) {
        if (secname.rfind(".text.", 0) == 0) {
            CuAsmSection* section = mSectionDict.at(secname).get();
            mCurrSection = section;
            parseKernelText(*section, range.first, range.second);
            section->updateResourceInfo();
            std::string kname = secname.substr(6);
            auto symidx = getSymbolIdx(kname);
            // Mirrors python's `regnumdict[symidx] = ...` where symidx is None for an unresolved
            // symbol: inserted under a null key (here -1, which can never be a real symtab index)
            // rather than dropped, so a later unresolved kernel overwrites an earlier one exactly
            // as a python dict would.
            regnumdict[symidx.value_or(-1)] = section->getRegNum();
        }
    }

    CuAsmSection* sec = mSectionDict.at(".nv.info").get();
    const std::string& bytes = sec->getData();
    CuNVInfo nvinfo(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), "sm_" + std::to_string(mArch->getVersionNumber()));
    mArch->setRegCountInNVInfo(nvinfo, regnumdict);
    std::vector<std::uint8_t> serialized = nvinfo.serialize();
    sec->setData(std::string(serialized.begin(), serialized.end()));
}

void CuAsmParser::Impl::parseKernelText(CuAsmSection& section, int lineStart, int lineEnd) {
    CuAsmLogger::logProcedure("Parsing kernel text of \"" + section.name + "\"...");

    CuKernelAssembler kasm(mCuInsAsmRepos.get(), *mArch);

    static const std::regex pTextLine(R"(\[([\w:-]+)\](.*))");

    int insIdx = 0;
    for (int lineidx = lineStart; lineidx < lineEnd; ++lineidx) {
        const std::string& line = mLines[lineidx];
        std::string nline = trim(CuAsmParser::stripComments(line));
        mLineNo = lineidx + 1;

        std::smatch mLbl;
        std::smatch mDir;
        if (nline.empty() || matchAtStart(nline, mLabelPattern, mLbl) || matchAtStart(nline, mDirectivePattern, mDir)) {
            continue;
        }

        std::smatch res;
        if (!matchAtStart(nline, pTextLine, res)) {
            doAssert(false, "Unrecognized code text!");
        }

        std::string ccodeS = res[1].str();
        std::string icodeS = res[2].str();

        if (!matchesControlCodePattern(ccodeS)) {
            doAssert(false, "Illegal control code text \"" + ccodeS + "\"!");
        }

        std::uint64_t addr = mArch->getInsOffsetFromIndex(insIdx);
        std::string cIcodeS = evalInstructionFixup(section, addr, icodeS);

        try {
            kasm.push(addr, cIcodeS, ccodeS);
        } catch (const std::exception& e) {
            doAssert(false, std::string("Error when assembling instruction \"") + nline + "\":\n        " + e.what());
        }

        ++insIdx;
    }

    std::string codebytes = kasm.genCode();
    section.seek(0, 0);
    section.emitBytes(codebytes);

    std::string kname = section.name.substr(6);
    CuAsmSection* infoSec = mSectionDict.at(".nv.info." + kname).get();

    std::map<std::string, CuNVInfoValue> offsetLabelDict;
    auto nvIt = mNVInfoOffsetLabels.find(kname);
    if (nvIt != mNVInfoOffsetLabels.end()) {
        for (const auto& [attr, offsets] : nvIt->second) {
            offsetLabelDict[attr] = offsets;
        }
    }
    for (const auto& [attr, val] : kasm.m_ExtraInfo) {
        offsetLabelDict[attr] = val;
    }

    const std::string& infoBytes = infoSec->getData();
    CuNVInfo nvinfo(std::vector<std::uint8_t>(infoBytes.begin(), infoBytes.end()), "sm_" + std::to_string(mArch->getVersionNumber()));
    nvinfo.updateNVInfoFromDict(offsetLabelDict);
    std::vector<std::uint8_t> serialized = nvinfo.serialize();
    infoSec->setData(std::string(serialized.begin(), serialized.end()));
}

void CuAsmParser::Impl::buildRelocationSections() {
    std::map<std::string, std::vector<CuAsmRelocation*>> relSecDict;
    std::vector<std::string> order;

    for (auto& rel : mRelList) {
        std::string sname = (rel->isRELA() ? ".rela" : ".rel") + rel->section->name;
        if (!relSecDict.count(sname)) {
            order.push_back(sname);
        }
        relSecDict[sname].push_back(rel.get());
    }

    for (const auto& sname : order) {
        CuAsmSection* section = mSectionDict.at(sname).get();
        auto& rellist = relSecDict.at(sname);
        for (auto it = rellist.rbegin(); it != rellist.rend(); ++it) {
            section->emitBytes((*it)->buildEntry());
        }
    }
}

void CuAsmParser::Impl::evalFixups() {
    static const std::regex pTexRel(R"(\[20@lo\(0x0\)=(\w+)\])");
    static const std::regex pSufRel(R"(\[20@lo\(0x0\)=fun@R_CUDA_SURF_HEADER_INDEX\((\w+)\)\])");
    static const std::regex pRel(R"(fun@(\w+)\(([^\)])\))");
    static const std::map<std::string, int> relDtypes = {{"dword", 0}, {"word", 1}};

    for (auto& fixupPtr : mFixupList) {
        CuAsmFixup& fixup = *fixupPtr;
        try {
            const std::string& expr = fixup.expr;

            if (!relDtypes.count(fixup.dtype) || expr.rfind("index@", 0) == 0) {
                ExprResult res = evalExpr(expr);
                fixup.value = res.value;
                updateSectionForFixup(fixup);
                continue;
            }

            bool handled = false;
            if (fixup.dtype == "word") {
                std::smatch m;
                if (matchAtStart(expr, pTexRel, m)) {
                    std::string symname = m[1].str();
                    auto relsymid = getSymbolIdx(symname);
                    mRelList.push_back(
                        std::make_unique<CuAsmRelocation>(fixup.section, fixup.offset, symname, relsymid, "R_CUDA_TEX_HEADER_INDEX"));
                    handled = true;
                } else {
                    std::smatch m2;
                    if (matchAtStart(expr, pSufRel, m2)) {
                        std::string symname = m2[1].str();
                        auto relsymid = getSymbolIdx(symname);
                        mRelList.push_back(std::make_unique<CuAsmRelocation>(fixup.section, fixup.offset, symname, relsymid,
                                                                              "R_CUDA_SURF_HEADER_INDEX"));
                        handled = true;
                    }
                }
            }
            if (handled) {
                continue;
            }

            std::smatch mRelMatch;
            if (matchAtStart(expr, pRel, mRelMatch)) {
                std::string reltype = mRelMatch[1].str();
                std::string symname = mRelMatch[2].str();
                auto symidx = getSymbolIdx(symname);
                mRelList.push_back(std::make_unique<CuAsmRelocation>(fixup.section, fixup.offset, symname, symidx, reltype));
                continue;
            }

            ExprResult res = evalExpr(expr);
            if (std::holds_alternative<std::string>(res.aReal)) {
                std::string symname = std::get<std::string>(res.aReal);
                auto relsymid = getSymbolIdx(symname);
                std::string reltype;
                if (fixup.dtype == "word") {
                    reltype = "R_CUDA_32";
                } else if (fixup.dtype == "dword") {
                    reltype = "R_CUDA_64";
                } else {
                    doAssert(false, "Unknown data type for relocation: " + fixup.dtype);
                }
                mRelList.push_back(std::make_unique<CuAsmRelocation>(fixup.section, fixup.offset, symname, relsymid, reltype));
            }

            fixup.value = res.value;
            updateSectionForFixup(fixup);
        } catch (const std::exception& e) {
            doAssert(false, "Error when evaluating fixup @line" + std::to_string(fixup.lineNo) + ": expr=" + fixup.expr +
                                 ", msg=" + e.what());
        }
    }
}

void CuAsmParser::Impl::updateSymtab() {
    std::string symtabBytes = mSectionDict.at(".symtab")->getData();
    const std::size_t symsize = sizeof(ELFIO::Elf64_Sym);

    std::size_t i = 0;
    for (auto& [s, idAndSym] : mSymtabDict) {
        ELFIO::Elf64_Sym& syment = idAndSym.second;

        if (mLabelDict.contains(s)) {
            syment.st_value = mLabelDict.at(s)->offset;

            if (mSymbolDict.contains(s)) {
                CuAsmSymbol& symobj = *mSymbolDict.at(s);
                symobj.value = mLabelDict.at(s)->offset;
                ExprResult res = evalExpr(*symobj.size);
                symobj.sizeval = static_cast<std::uint64_t>(res.value);
                syment.st_size = *symobj.sizeval;

                CuAsmSymbol::resetSymtabEntryValueSize(symtabBytes, i * symsize, *symobj.value, *symobj.sizeval);
            }
        }
        ++i;
    }
    mSectionDict.at(".symtab")->setData(symtabBytes);
}

void CuAsmParser::Impl::layoutSections() {
    struct Edge {
        std::uint64_t fileStart = 0;
        std::uint64_t fileEnd = 0;
        std::uint64_t memStart = 0;
        std::uint64_t memEnd = 0;
    };

    std::uint64_t elfheadersize = sizeof(ELFIO::Elf64_Ehdr);
    std::uint64_t fileOffset = elfheadersize;
    std::uint64_t memOffset = elfheadersize;
    CuAsmSection* prevSec = nullptr;

    std::map<std::string, Edge> shEdges;

    for (auto& [secname, secPtr] : mSectionDict) {
        if (secname.empty()) {
            continue;
        }
        CuAsmSection& sec = *secPtr;

        std::uint64_t align = sec.addralign;
        if (prevSec != nullptr && prevSec->name.rfind(".text", 0) == 0) {
            align = 128;
        }
        auto [newFileOffset, newMemOffset] = updateSectionPadding(prevSec, fileOffset, memOffset, align);
        fileOffset = newFileOffset;
        memOffset = newMemOffset;

        sec.size = sec.getDataSize();
        sec.offset = fileOffset;

        sec.header["size"] = static_cast<std::int64_t>(sec.size);
        sec.header["offset"] = static_cast<std::int64_t>(sec.offset);

        prevSec = &sec;
        shEdges[secname] = Edge{fileOffset, 0, memOffset, 0};

        memOffset += sec.size;
        bool isNobits = sec.header.count("type") && isHeaderTypeNamed(sec.header.at("type"), "SHT_NOBITS");
        if (!isNobits) {
            fileOffset += sec.size;
        }
    }

    if (prevSec != nullptr && prevSec->name.rfind(".text", 0) == 0) {
        auto [newFileOffset, newMemOffset] = updateSectionPadding(prevSec, fileOffset, memOffset, 128);
        fileOffset = newFileOffset;
        memOffset = newMemOffset;
    }

    for (auto& [secname, secPtr] : mSectionDict) {
        if (secname.empty()) {
            continue;
        }
        CuAsmSection& sec = *secPtr;

        sec.size = sec.getDataSize();
        sec.header["size"] = static_cast<std::int64_t>(sec.size);

        bool isNobits = sec.header.count("type") && isHeaderTypeNamed(sec.header.at("type"), "SHT_NOBITS");
        std::uint64_t fsize = isNobits ? 0 : sec.size;
        std::uint64_t msize = sec.size;

        Edge& e = shEdges.at(secname);
        e.fileEnd = e.fileStart + fsize;
        e.memEnd = e.memStart + msize;
    }

    auto [alignedFileOffset, padBeforeHeader] = alignTo(fileOffset, 8);
    fileOffset = alignedFileOffset;
    mPadSizeBeforeSecHeader = padBeforeHeader;

    std::uint64_t secHeaderLen = mSectionDict.size() * sizeof(ELFIO::Elf64_Shdr);

    mCuAsmFile.fileHeader["shoff"] = static_cast<std::int64_t>(fileOffset);

    std::uint64_t phoff = fileOffset + secHeaderLen;
    std::int64_t phentsize = resolveHeaderInt(mCuAsmFile.fileHeader.at("phentsize"));
    std::int64_t phnum = resolveHeaderInt(mCuAsmFile.fileHeader.at("phnum"));
    std::uint64_t phlen = static_cast<std::uint64_t>(phentsize * phnum);
    mCuAsmFile.fileHeader["phoff"] = static_cast<std::int64_t>(phoff);

    shEdges[PROGRAM_HEADER_TAG] = Edge{phoff, phoff + phlen, phoff, phoff + phlen};

    for (auto& segPtr : mSegmentList) {
        CuAsmSegment& seg = *segPtr;
        std::string segType = std::get<std::string>(seg.header.at("type"));

        if (segType == "PT_PHDR") {
            seg.header["offset"] = static_cast<std::int64_t>(fileOffset + secHeaderLen);
            std::int64_t filesz = static_cast<std::int64_t>(sizeof(ELFIO::Elf64_Phdr) * mSegmentList.size());
            seg.header["filesz"] = filesz;
            seg.header["memsz"] = filesz;
        } else if (segType == "PT_LOAD") {
            std::string startsection = seg.header.count("startsection") ? std::get<std::string>(seg.header.at("startsection")) : "";
            std::string endsection = seg.header.count("endsection") ? std::get<std::string>(seg.header.at("endsection")) : "";
            if (!startsection.empty() && !endsection.empty()) {
                const Edge& e0 = shEdges.at(startsection);
                const Edge& e1 = shEdges.at(endsection);

                seg.header["offset"] = static_cast<std::int64_t>(e0.fileStart);
                seg.header["filesz"] = static_cast<std::int64_t>(e1.fileEnd - e0.fileStart);
                seg.header["memsz"] = static_cast<std::int64_t>(e1.memEnd - e0.memStart);
            }
        } else {
            std::string msg = "Unknown segment type " + segType + "!";
            CuAsmLogger::logError(msg);
            throw std::runtime_error(msg);
        }

        seg.updateHeader();
    }
}

void CuAsmParser::Impl::dirHeaderflags(const std::vector<std::string>& args) {
    assertArgc(".headerflags", args, 1, false);
    mCuAsmFile.headerflags = args[0];
}

void CuAsmParser::Impl::dirElftype(const std::vector<std::string>& args) {
    assertArgc(".elftype", args, 1, false);
    mCuAsmFile.elftype = args[0];
}

void CuAsmParser::Impl::dirSection(const std::vector<std::string>& args) {
    assertArgc(".section", args, 3, false);

    std::string secname = stripQuotes(args[0]);
    doAssert(!mSectionDict.contains(secname), "Redefinition of section \"" + secname + "\"!");

    auto newSection = std::make_unique<CuAsmSection>(secname, args[1], args[2]);
    mCurrSection = newSection.get();

    CuAsmLogger::logSubroutine("Line " + std::to_string(mLineNo) + ": New section \"" + secname + "\"");

    mSectionDict[secname] = std::move(newSection);

    if (args[0].rfind(".text.", 0) == 0) {
        mInTextSection = true;
        mInsIndex = 0;
    } else {
        mInTextSection = false;
    }
}

void CuAsmParser::Impl::dirSectionflags(const std::vector<std::string>& args) {
    assertArgc(".sectionflags", args, 1, false);
    mCurrSection->flags.push_back(args[0]);
}

void CuAsmParser::Impl::dirSectionentsize(const std::vector<std::string>& args) {
    assertArgc(".sectionentsize", args, 1, false);
    mCurrSection->entsize = static_cast<std::uint64_t>(std::stoll(args[0]));
}

void CuAsmParser::Impl::dirSectioninfo(const std::vector<std::string>& args) {
    assertArgc(".sectioninfo", args, 1, false);
    doAssert(mCurrSection != nullptr, "No active section!");
    mCurrSection->info.push_back(args[0]);
}

void CuAsmParser::Impl::dirByte(const std::vector<std::string>& args) {
    assertArgc(".word", args, 1, true);
    emitTypedBytes("byte", args);
}

void CuAsmParser::Impl::dirDword(const std::vector<std::string>& args) {
    assertArgc(".dword", args, 1, true);
    emitTypedBytes("dword", args);
}

void CuAsmParser::Impl::dirAlign(const std::vector<std::string>& args) {
    assertArgc(".align", args, 1, false);
    int align = 0;
    bool ok = true;
    try {
        align = std::stoi(args[0]);
    } catch (...) {
        ok = false;
    }
    doAssert(ok, " unknown alignment (" + args[0] + ")!");
    doAssert((align & (align - 1)) == 0, " alignment(" + std::to_string(align) + ") should be power of 2!");
    mCurrSection->emitAlign(align);
}

void CuAsmParser::Impl::dirShort(const std::vector<std::string>& args) {
    assertArgc(".short", args, 1, true);
    emitTypedBytes("short", args);
}

void CuAsmParser::Impl::dirWord(const std::vector<std::string>& args) {
    assertArgc(".word", args, 1, true);
    emitTypedBytes("word", args);
}

void CuAsmParser::Impl::dirType(const std::vector<std::string>& args) {
    assertArgc(".type", args, 2, false);
    const std::string& symbol = args[0];
    if (!mSymbolDict.contains(symbol)) {
        mSymbolDict[symbol] = std::make_unique<CuAsmSymbol>(symbol);
    }
    const std::string& stype = args[1];
    doAssert(CuAsmSymbol::symbolTypes().count(stype) != 0, "Unknown symbol type " + stype + "!");
    mSymbolDict.at(symbol)->type = stype;
}

void CuAsmParser::Impl::dirSize(const std::vector<std::string>& args) {
    assertArgc(".size", args, 2, false);
    const std::string& symbol = args[0];
    if (!mSymbolDict.contains(symbol)) {
        mSymbolDict[symbol] = std::make_unique<CuAsmSymbol>(symbol);
    }
    mSymbolDict.at(symbol)->size = args[1];
}

void CuAsmParser::Impl::dirGlobal(const std::vector<std::string>& args) {
    assertArgc(".global", args, 1, false);
    const std::string& symbol = args[0];
    if (!mSymbolDict.contains(symbol)) {
        mSymbolDict[symbol] = std::make_unique<CuAsmSymbol>(symbol);
    }
    CuAsmLogger::logSubroutine("Line " + std::to_string(mLineNo) + " global symbol " + symbol);
    mSymbolDict.at(symbol)->isGlobal = true;
}

void CuAsmParser::Impl::dirWeak(const std::vector<std::string>& args) {
    assertArgc(".weak", args, 1, false);
    const std::string& symbol = args[0];
    if (!mSymbolDict.contains(symbol)) {
        mSymbolDict[symbol] = std::make_unique<CuAsmSymbol>(symbol);
    }
    CuAsmLogger::logWarning("Line " + std::to_string(mLineNo) +
                             ": Weak symbol found! The implementation is not complete, please be cautious...");
    CuAsmLogger::logSubroutine("Line " + std::to_string(mLineNo) + ": New weak symbol \"" + symbol + "\"");
    mSymbolDict.at(symbol)->isGlobal = true;
}

void CuAsmParser::Impl::dirZero(const std::vector<std::string>& args) {
    assertArgc(".zero", args, 1, false);
    bool ok = true;
    int size = 0;
    try {
        size = std::stoi(args[0]);
        if (size > 0) {
            emitBytes(std::string(static_cast<std::size_t>(size), '\0'));
        }
    } catch (...) {
        ok = false;
    }
    doAssert(ok, "Unknown arg (" + args[0] + ") for .zero!");
}

void CuAsmParser::Impl::dirOther(const std::vector<std::string>& args) {
    assertArgc(".other", args, 2, false);
    const std::string& symbol = args[0];
    doAssert(mSymbolDict.contains(symbol), "Undefined symbol " + symbol + "!!!");
    mSymbolDict.at(symbol)->other = args[1];
}

void CuAsmParser::Impl::dirElfheader(const std::string& attrname, const std::vector<std::string>& args) {
    assertArgc(".__elf_" + attrname, args, 1, false);
    mCuAsmFile.fileHeader[attrname] = cvtValue(args[0]);

    if (attrname == "flags") {
        std::int64_t flags = std::stoll(args[0], nullptr, 16);
        int smversion = static_cast<int>(flags & 0xff);
        mArch = std::make_unique<CuSMVersion>(smversion);

        if (!mCuInsAsmRepos || mCuInsAsmRepos->getSMVersion() != *mArch) {
            CuAsmLogger::logSubroutine("Setting CuInsAsmRepos to default dict...");
            mCuInsAsmRepos = std::make_unique<CuInsAssemblerRepos>(*mArch);
            mCuInsAsmRepos->setToDefaultInsAsmDict();
        }
    }
}

void CuAsmParser::Impl::dirSectionheader(const std::string& attrname, const std::vector<std::string>& args) {
    assertArgc(".__section_" + attrname, args, 1, false);
    mCurrSection->header[attrname] = cvtValue(args[0]);
}

void CuAsmParser::Impl::dirSegment(const std::vector<std::string>& args) {
    assertArgc(".__segment", args, 2, false);
    auto seg = std::make_unique<CuAsmSegment>(stripQuotes(args[0]), args[1]);
    mCurrSegment = seg.get();
    mSegmentList.push_back(std::move(seg));
    mCurrSection = nullptr;
}

void CuAsmParser::Impl::dirSegmentheader(const std::string& attrname, const std::vector<std::string>& args) {
    assertArgc(".__segment_" + attrname, args, 1, false);
    mCurrSegment->header[attrname] = cvtValue(args[0]);
}

void CuAsmParser::Impl::doAssert(bool flag, const std::string& msg) {
    if (flag) {
        return;
    }
    std::ostringstream full;
    full << "Assertion failed in:\n";
    full << "    File " << mFilename << ":" << mLineNo << " :\n";
    std::string lineText = (mLineNo >= 1 && static_cast<std::size_t>(mLineNo) <= mLines.size()) ? trim(mLines[mLineNo - 1]) : "";
    full << "        " << lineText << "\n";
    full << "    " << msg;
    CuAsmLogger::logError(full.str());
    throw std::runtime_error(full.str());
}

void CuAsmParser::Impl::assertArgc(const std::string& cmd, const std::vector<std::string>& args, std::size_t argc, bool allowMore) {
    bool flag = allowMore ? args.size() >= argc : args.size() == argc;
    std::string es = allowMore ? "at least " : "";

    std::ostringstream argsStr;
    argsStr << "[";
    for (std::size_t i = 0; i < args.size(); ++i) {
        argsStr << args[i];
        if (i + 1 < args.size()) {
            argsStr << ", ";
        }
    }
    argsStr << "]";

    doAssert(flag, cmd + " requires " + es + std::to_string(argc) + " args! " + std::to_string(args.size()) + " given: " +
                        argsStr.str() + ".");
}

std::uint64_t CuAsmParser::Impl::tellLocal() {
    if (mCurrSection == nullptr) {
        throw std::runtime_error("Cannot tell local pos without active section!");
    }
    return mCurrSection->tell();
}

std::optional<int> CuAsmParser::Impl::getSymbolIdx(const std::string& symname) {
    if (mSymtabDict.contains(symname)) {
        return mSymtabDict.at(symname).first;
    }
    return std::nullopt;
}

std::pair<std::int64_t, bool> CuAsmParser::Impl::evalVar(const std::string& var) {
    bool isSym = mSymtabDict.contains(var);

    static const std::regex intval(R"(^(0x[a-fA-F0-9]+|[0-9]+))");
    if (std::regex_search(var, intval)) {
        return {parsePyIntLiteral(var), isSym};
    }

    static const std::string srelSuffix = "@srel";
    if (var.size() > srelSuffix.size() && var.compare(var.size() - srelSuffix.size(), srelSuffix.size(), srelSuffix) == 0) {
        std::string label = var.substr(0, var.size() - srelSuffix.size());
        if (!mLabelDict.contains(label)) {
            throw std::runtime_error("Unknown expression " + var);
        }
        return {static_cast<std::int64_t>(mLabelDict.at(label)->offset), isSym};
    }

    if (mLabelDict.contains(var)) {
        return {static_cast<std::int64_t>(mLabelDict.at(var)->offset), isSym};
    }

    throw std::runtime_error("Unknown expression " + var);
}

ExprResult CuAsmParser::Impl::evalExpr(const std::string& expr) {
    if (expr.rfind("index@", 0) == 0) {
        std::string symname = stripChars(expr.substr(6), " ()");
        auto index = getSymbolIdx(symname);
        doAssert(index.has_value(), "Unknown symbol \"" + symname + "\"!!!");
        ExprResult r;
        r.value = *index;
        r.aReal = static_cast<std::int64_t>(*index);
        return r;
    }

    std::string rexpr = stripChars(expr, "`() ");
    static const std::regex pExpr(R"(([.\w$@]+)\s*([+-])*\s*([.\w$@]+)*)");
    std::smatch m;
    doAssert(matchAtStart(rexpr, pExpr, m), "Unknown expr " + expr + " !!!");

    std::string a = m[1].str();
    std::string opStr = m[2].str();
    std::string b = m[3].str();

    auto [aval, aIsSym] = evalVar(a);

    ExprResult r;
    if (opStr.empty()) {
        r.value = aval;
        r.aReal = aIsSym ? std::variant<std::int64_t, std::string>(a) : std::variant<std::int64_t, std::string>(aval);
        return r;
    }

    auto [bval, bIsSym] = evalVar(b);
    (void)bIsSym;

    r.aReal = aIsSym ? std::variant<std::int64_t, std::string>(a) : std::variant<std::int64_t, std::string>(aval);

    char op = opStr[0];
    if (op == '+') {
        r.value = aval + bval;
    } else if (op == '-') {
        r.value = aval - bval;
    } else {
        throw std::runtime_error("Unknown expr.op");
    }
    r.op = op;
    r.bval = bval;
    return r;
}

std::string CuAsmParser::Impl::evalInstructionFixup(CuAsmSection& section, std::uint64_t offset, const std::string& s) {
    static const std::regex pInsRel32(R"((32@hi|32@lo)\(([^\)]+)\)+)");
    std::smatch r1;
    if (std::regex_search(s, r1, pInsRel32)) {
        std::string expr = r1[2].str();
        ExprResult res = evalExpr(expr);
        std::string symname = std::holds_alternative<std::string>(res.aReal) ? std::get<std::string>(res.aReal) : std::string();
        auto symidx = symname.empty() ? std::optional<int>(std::nullopt) : getSymbolIdx(symname);
        std::string relkey = r1[1].str();
        std::string reltype = mArch->getInsRelocationType(relkey);

        if (res.op.has_value()) {
            mRelList.push_back(std::make_unique<CuAsmRelocation>(&section, offset, symname, symidx, reltype, *res.bval));
        } else {
            mRelList.push_back(std::make_unique<CuAsmRelocation>(&section, offset, symname, symidx, reltype));
        }

        return std::regex_replace(s, pInsRel32, "0x0");
    }

    static const std::regex pInsLabel(R"(`\(([^\)]+)\))");
    std::smatch r2;
    if (std::regex_search(s, r2, pInsLabel)) {
        std::string label = r2[1].str();
        doAssert(mLabelDict.contains(label) || mSymtabDict.contains(label), "Unknown label (" + label + ") !!!");

        if (!mLabelDict.contains(label) && mSymtabDict.contains(label)) {
            std::string symname = label;
            auto symidx = getSymbolIdx(symname);
            std::string reltype = mArch->getInsRelocationType("target");
            mRelList.push_back(std::make_unique<CuAsmRelocation>(&section, offset, symname, symidx, reltype));
            return std::regex_replace(s, pInsLabel, "0x0");
        }

        CuAsmLabel* clabel = mLabelDict.at(label).get();
        if (section.name == clabel->section->name) {
            std::ostringstream v;
            v << "0x" << std::hex << clabel->offset;
            return std::regex_replace(s, pInsLabel, v.str());
        }

        std::string symname = label;
        auto symidx = getSymbolIdx(symname);
        std::string reltype = mArch->getInsRelocationType("target");
        mRelList.push_back(std::make_unique<CuAsmRelocation>(&section, offset, symname, symidx, reltype));
        return std::regex_replace(s, pInsLabel, "0x0");
    }

    return s;
}

namespace {

/** @brief Packs an unsigned magnitude into `size` little-endian bytes, mirroring python's
 *         int.to_bytes(value, size, 'little') (signed=False default), which raises OverflowError
 *         if `value` doesn't fit unsigned in `size` bytes.
 * @param value Magnitude to pack.
 * @param size Number of bytes to pack into.
 * @return The packed little-endian bytes.
 **/
std::string packUnsignedLE(std::uint64_t value, int size) {
    if (size < 8 && value >= (std::uint64_t{1} << (8 * size))) {
        throw std::overflow_error("int too big to convert");
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    for (int i = 0; i < size; ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<char>((value >> (8 * i)) & 0xff);
    }
    return bytes;
}

} // namespace

void CuAsmParser::Impl::updateSectionForFixup(CuAsmFixup& fixup) {
    static const std::map<std::string, int> dtypeSize = {{"byte", 1}, {"short", 2}, {"word", 4}, {"dword", 8}};
    int blen = dtypeSize.at(fixup.dtype);
    std::int64_t value = *fixup.value;
    if (value < 0) {
        throw std::overflow_error("can't convert negative int to unsigned");
    }
    std::uint64_t v = static_cast<std::uint64_t>(value);
    std::string bs = packUnsignedLE(v, blen);
    fixup.section->updateForFixup(fixup.offset, bs);

    CuAsmLogger::logSubroutine("Eval fixup \"" + fixup.expr + "\" @line" + std::to_string(fixup.lineNo) + " to 0x" +
                                [v] { std::ostringstream o; o << std::hex << v; return o.str(); }());
}

void CuAsmParser::Impl::emitBytes(const std::string& bs) { mCurrSection->emitBytes(bs); }

CuAsmParser::Impl::LineType CuAsmParser::Impl::getLineType(const std::string& line) {
    std::smatch m;
    if (line.empty()) {
        return LineType::Blank;
    }
    if (matchAtStart(line, mLabelPattern, m)) {
        return LineType::Label;
    }
    if (matchAtStart(line, mDirectivePattern, m)) {
        return LineType::Directive;
    }
    if (mInTextSection) {
        return LineType::Code;
    }
    return LineType::Unknown;
}

void CuAsmParser::Impl::emitTypedBytes(const std::string& dtype, const std::vector<std::string>& args) {
    static const std::map<std::string, int> dtypeSize = {{"byte", 1}, {"short", 2}, {"word", 4}, {"dword", 8}};
    int dsize = dtypeSize.at(dtype);

    for (const auto& arg : args) {
        if (arg.rfind("0x", 0) == 0) {
            std::uint64_t argv = std::stoull(arg, nullptr, 16);
            try {
                emitBytes(packUnsignedLE(argv, dsize));
            } catch (const std::overflow_error&) {
                doAssert(false, "Value " + arg + " does not fit in ." + dtype + "!");
            }
        } else {
            auto fixup = std::make_unique<CuAsmFixup>(mCurrSection, tellLocal(), arg, dtype, mLineNo);
            mFixupList.push_back(std::move(fixup));
            emitBytes(std::string(static_cast<std::size_t>(dsize), '\0'));
        }
    }
}

std::string CuAsmParser::Impl::genSectionPaddingBytes(CuAsmSection& sec, std::uint64_t padSize) {
    std::string padbytes = sec.name.rfind(".text", 0) == 0 ? mArch->getPadBytes() : std::string(1, '\0');
    if (padbytes.empty() || padSize % padbytes.size() != 0) {
        throw std::runtime_error("Invalid padding size for section " + sec.name);
    }
    std::uint64_t npad = padSize / padbytes.size();
    std::string out;
    out.reserve(npad * padbytes.size());
    for (std::uint64_t i = 0; i < npad; ++i) {
        out += padbytes;
    }
    return out;
}

std::pair<std::uint64_t, std::uint64_t> CuAsmParser::Impl::updateSectionPadding(CuAsmSection* sec, std::uint64_t fileOffset,
                                                                                  std::uint64_t memOffset, std::uint64_t align) {
    if (sec == nullptr) {
        return {fileOffset, memOffset};
    }

    if (sec->name.rfind(".text", 0) == 0) {
        align = std::max(align, sec->addralign);
        auto [newFileOffset, fpadsize] = alignTo(fileOffset, align);
        auto [newMemOffset, mpadsize] = alignTo(memOffset, align);
        (void)mpadsize;

        sec->emitPadding(genSectionPaddingBytes(*sec, fpadsize));

        if (mSecSizeLabel.count(sec->name)) {
            CuAsmLabel* sizelabel = mSecSizeLabel.at(sec->name);
            sizelabel->offset = sec->getDataSize();
            CuAsmLogger::logSubroutine("Reset size label \"" + sizelabel->name + "\" of " + sec->name + " to " +
                                        std::to_string(sec->getDataSize()) + "!");
        }

        sec->updateHeader();
        return {newFileOffset, newMemOffset};
    }

    bool isNobits = sec->header.count("type") && isHeaderTypeNamed(sec->header.at("type"), "SHT_NOBITS");
    if (isNobits) {
        auto [newMemOffset, mpadsize] = alignTo(memOffset, align);
        sec->padsize = mpadsize;
        sec->padbytes = std::string(mpadsize, '\0');
        sec->updateHeader();
        return {fileOffset, newMemOffset};
    }

    auto [newFileOffset, fpadsize] = alignTo(fileOffset, align);
    auto [newMemOffset, mpadsize] = alignTo(memOffset, align);
    sec->padsize = fpadsize;
    sec->padbytes = std::string(fpadsize, '\0');
    sec->updateHeader();
    return {newFileOffset, newMemOffset};
}

std::string CuAsmParser::Impl::checkNVInfoOffsetLabels(CuAsmSection* section, const std::string& labelname, std::uint64_t offset) {
    if (labelname.rfind(".CUASM_OFFSET_LABEL", 0) != 0) {
        return labelname;
    }

    doAssert(section != nullptr && section->name.rfind(".text", 0) == 0, "CUASM_OFFSET_LABEL should be defined in a text section!");

    std::string kname = section->name.substr(6);
    std::vector<std::string> vs = splitOn(labelname.substr(1), '.');
    doAssert(vs.size() == 4, "Offset label should be in form: .CUASM_OFFSET_LABEL.{SectionName}.{NVInfoAttributeName}.{Identifier}");
    doAssert(vs[1] == kname, "CUASM_OFFSET_LABEL should include kernel name in second dot part!");

    const std::string& attr = vs[2];
    mNVInfoOffsetLabels[kname][attr].push_back(static_cast<std::uint32_t>(offset));

    if (vs[3] == "#") {
        std::ostringstream lstr;
        lstr << 'L' << std::hex << std::setw(8) << std::setfill('0') << mLineNo;
        return labelname.substr(0, labelname.size() - 1) + lstr.str();
    }
    return labelname;
}

void CuAsmParser::Impl::parse(const std::string& fname) {
    reset();
    CuAsmLogger::logEntry("Parsing file " + fname);

    mFilename = fname;
    doAssert(std::filesystem::is_regular_file(fname), "Cannot find input cuasm file " + fname + "!!!");

    mLines.clear();
    std::ifstream fin(fname);
    std::string lineStr;
    while (std::getline(fin, lineStr)) {
        mLines.push_back(lineStr);
    }

    CuAsmLogger::logTraceIt("CuAsmParser::preScan", [this] { preScan(); })();
    CuAsmLogger::logTraceIt("CuAsmParser::gatherTextSectionSizeLabel", [this] { gatherTextSectionSizeLabel(); })();

    CuAsmLogger::logTraceIt("CuAsmParser::buildInternalTables", [this] { buildInternalTables(); })();
    CuAsmLogger::logTraceIt("CuAsmParser::evalFixups", [this] { evalFixups(); })();
    CuAsmLogger::logTraceIt("CuAsmParser::parseKernels", [this] { parseKernels(); })();

    CuAsmLogger::logTraceIt("CuAsmParser::buildRelocationSections", [this] { buildRelocationSections(); })();

    CuAsmLogger::logTraceIt("CuAsmParser::layoutSections", [this] { layoutSections(); })();

    CuAsmLogger::logTraceIt("CuAsmParser::updateSymtab", [this] { updateSymtab(); })();
}

void CuAsmParser::Impl::saveAsCubin(const std::string& path) {
    std::ofstream fout(path, std::ios::binary);
    CuAsmLogger::logEntry("Saving as cubin file " + path + "...");

    auto disppos = [&](const std::string& s) {
        auto pos = static_cast<std::uint64_t>(fout.tellp());
        std::ostringstream o;
        o << "0x" << std::hex << std::setw(8) << std::setfill('0') << pos << std::dec << "(" << std::setw(8)
          << std::setfill('0') << pos << ") : " << s;
        CuAsmLogger::logSubroutine(o.str());
    };

    disppos("FileHeader");
    std::string hdr = mCuAsmFile.buildFileHeader();
    fout.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));

    for (auto& [sname, secPtr] : mSectionDict) {
        disppos("SectionData " + sname);
        secPtr->writePaddedData(fout);
    }

    if (mPadSizeBeforeSecHeader > 0) {
        disppos("Padding " + std::to_string(mPadSizeBeforeSecHeader) + " bytes before section header");
        std::string pad(mPadSizeBeforeSecHeader, '\0');
        fout.write(pad.data(), static_cast<std::streamsize>(pad.size()));
    }

    for (auto& [sname, secPtr] : mSectionDict) {
        disppos("SectionHeader " + sname);
        std::string hdrBytes = secPtr->buildHeader();
        fout.write(hdrBytes.data(), static_cast<std::streamsize>(hdrBytes.size()));
    }

    for (auto& segPtr : mSegmentList) {
        disppos("Segment");
        std::string segBytes = segPtr->build();
        fout.write(segBytes.data(), static_cast<std::streamsize>(segBytes.size()));
    }
}

void CuAsmParser::Impl::saveCubinCmp(const std::string& cubinName, const std::string& savPrefix) {
    std::ofstream fasm(savPrefix + "_asm.txt");
    std::ofstream fbin(savPrefix + "_bin.txt");

    ELFIO::elfio reader;
    if (!reader.load(cubinName)) {
        throw std::runtime_error("Cannot load reference cubin " + cubinName);
    }

    fasm << "FileHeader:\n" << dumpEhdr(mCuAsmFile.getFileHeaderStruct()) << "\n";
    fbin << "FileHeader:\ne_type=" << reader.get_type() << " e_machine=" << reader.get_machine()
         << " e_entry=0x" << std::hex << reader.get_entry() << std::dec << "\n";

    for (auto& [sname, secPtr] : mSectionDict) {
        fasm << "# Section " << sname << "\n";
        fasm << dumpShdr(secPtr->getHeaderStruct()) << "\n";
        bool isNobits = secPtr->header.count("type") && resolveHeaderInt(secPtr->header.at("type")) == ELFIO::SHT_NOBITS;
        if (!isNobits) {
            fasm << bytes2Asm(secPtr->getData()) << "\n\n";
        } else {
            fasm << "\n";
        }
    }

    for (auto& segPtr : mSegmentList) {
        fasm << dumpPhdr(segPtr->getHeaderStruct()) << "\n";
    }

    ELFIO::Elf_Half numSections = reader.sections.size();
    for (ELFIO::Elf_Half i = 0; i < numSections; ++i) {
        const ELFIO::section* sec = reader.sections[i];
        fbin << "# Section " << sec->get_name() << "\n";
        fbin << "sh_type=" << sec->get_type() << " sh_flags=0x" << std::hex << sec->get_flags() << " sh_offset=0x"
             << sec->get_offset() << std::dec << " sh_size=" << sec->get_size() << " sh_link=" << sec->get_link()
             << " sh_info=" << sec->get_info() << " sh_addralign=" << sec->get_addr_align()
             << " sh_entsize=" << sec->get_entry_size() << "\n";
        if (sec->get_type() != ELFIO::SHT_NOBITS) {
            fbin << bytes2Asm(std::string(sec->get_data(), sec->get_size())) << "\n\n";
        } else {
            fbin << "\n";
        }
    }

    ELFIO::Elf_Half numSegments = reader.segments.size();
    for (ELFIO::Elf_Half i = 0; i < numSegments; ++i) {
        const ELFIO::segment* seg = reader.segments[i];
        fbin << "p_type=" << seg->get_type() << " p_flags=" << seg->get_flags() << " p_offset=0x" << std::hex
             << seg->get_offset() << " p_vaddr=0x" << seg->get_virtual_address() << " p_paddr=0x"
             << seg->get_physical_address() << std::dec << " p_filesz=" << seg->get_file_size()
             << " p_memsz=" << seg->get_memory_size() << " p_align=" << seg->get_align() << "\n";
    }
}

void CuAsmParser::Impl::dispFixupList() const {
    std::cout << "Fixup list:\n";
    if (mFixupList.empty()) {
        std::cout << "    []\n";
    }
    for (std::size_t i = 0; i < mFixupList.size(); ++i) {
        std::cout << "Fixup " << std::setw(3) << i << ": " << mFixupList[i]->toString() << "\n";
    }
    std::cout << std::endl;
}

void CuAsmParser::Impl::dispRelocationList() const {
    std::cout << "Relocation list:\n";
    if (mRelList.empty()) {
        std::cout << "    No relocations.\n";
    }
    for (std::size_t i = 0; i < mRelList.size(); ++i) {
        std::cout << "Relocation " << std::setw(3) << i << ": " << mRelList[i]->toString() << "\n";
    }
    std::cout << std::endl;
}

void CuAsmParser::Impl::dispSectionList() const {
    std::cout << "Section list:\n";
    if (mSectionDict.empty()) {
        std::cout << "    No sections found.\n";
        return;
    }

    std::cout << " Idx  Offset    Size    ES   AL  Type           Flags    Link      Info  Name\n";
    std::size_t i = 0;
    for (const auto& [sname, secPtr] : mSectionDict) {
        const ELFIO::Elf64_Shdr& h = secPtr->getHeaderStruct();
        std::cout << std::hex << std::setw(4) << i << "  " << std::setw(6) << h.sh_offset << "  " << std::setw(6) << h.sh_size
                  << "  " << std::setw(4) << h.sh_entsize << "  " << std::setw(3) << secPtr->addralign << "  " << std::setw(12)
                  << h.sh_type << "  " << std::setw(6) << h.sh_flags << "  " << std::setw(6) << h.sh_link << "  " << std::setw(8)
                  << h.sh_info << std::dec << "  " << sname << "\n";
        ++i;
    }
    std::cout << std::endl;
}

void CuAsmParser::Impl::dispSymbolDict() const {
    std::cout << "\nSymbols:\n";
    std::size_t i = 0;
    for (const auto& [sname, symPtr] : mSymbolDict) {
        std::cout << "Symbol " << std::setw(3) << i << ": " << symPtr->toString() << "\n";
        ++i;
    }
    std::cout << std::endl;
}

void CuAsmParser::Impl::dispSymtabDict() const {
    std::cout << "\nSymtab:\n";
    for (const auto& [s, idAndSym] : mSymtabDict) {
        std::cout << "Symbol " << std::setw(3) << idAndSym.first << " (" << s << ")\n";
        if (mSymbolDict.contains(s)) {
            std::cout << "    " << mSymbolDict.at(s)->toString() << "\n";
        }
    }
    std::cout << std::endl;
}

void CuAsmParser::Impl::dispLabelDict() const {
    std::cout << "\nLabels: \n";
    std::size_t i = 0;
    for (const auto& [lname, labelPtr] : mLabelDict) {
        std::cout << "Label " << std::setw(3) << i << ": " << labelPtr->toString() << "\n";
        ++i;
    }
    std::cout << std::endl;
}

void CuAsmParser::Impl::dispSegmentHeader() const {
    std::cout << "Segment headers:\n";
    for (const auto& segPtr : mSegmentList) {
        std::cout << dumpPhdr(segPtr->getHeaderStruct()) << "\n";
    }
}

void CuAsmParser::Impl::dispFileHeader() const {
    std::cout << "File header:\n" << dumpEhdr(mCuAsmFile.getFileHeaderStruct()) << std::endl;
}

void CuAsmParser::Impl::dispTables() const {
    std::cout << ".shstrtab:\n";
    for (const auto& [idx, str] : mShstrtabDict) {
        std::cout << "0x" << std::hex << idx << std::dec << "\t" << str << "\n";
    }
    std::cout << ".strtab:\n";
    for (const auto& [idx, str] : mStrtabDict) {
        std::cout << "0x" << std::hex << idx << std::dec << "\t" << str << "\n";
    }
    std::cout << ".symtab\n";
    std::size_t i = 0;
    for (const auto& [s, idAndSym] : mSymtabDict) {
        (void)idAndSym;
        std::cout << std::setw(3) << i << "\t" << s << "\n";
        ++i;
    }
}

// ---- CuAsmParser: thin public wrapper delegating to Impl ----

CuAsmParser::CuAsmParser() : m_impl(std::make_unique<Impl>()) {}
CuAsmParser::~CuAsmParser() = default;
CuAsmParser::CuAsmParser(CuAsmParser&&) noexcept = default;
CuAsmParser& CuAsmParser::operator=(CuAsmParser&&) noexcept = default;

void CuAsmParser::setInsAsmRepos(const std::string& fname, const std::string& arch) { m_impl->setInsAsmRepos(fname, arch); }

void CuAsmParser::parse(const std::string& fname) {
    CuAsmLogger::logTimeIt("CuAsmParser::parse", [this, fname] { m_impl->parse(fname); })();
}

void CuAsmParser::saveAsCubin(const std::string& binFileName) {
    CuAsmLogger::logTimeIt("CuAsmParser::saveAsCubin", [this, binFileName] { m_impl->saveAsCubin(binFileName); })();
}

void CuAsmParser::saveCubinCmp(const std::string& cubinName, const std::string& savPrefix) {
    CuAsmLogger::logTimeIt("CuAsmParser::saveCubinCmp",
                            [this, cubinName, savPrefix] { m_impl->saveCubinCmp(cubinName, savPrefix); })();
}

void CuAsmParser::dispFixupList() const { m_impl->dispFixupList(); }
void CuAsmParser::dispRelocationList() const { m_impl->dispRelocationList(); }
void CuAsmParser::dispSectionList() const { m_impl->dispSectionList(); }
void CuAsmParser::dispSymbolDict() const { m_impl->dispSymbolDict(); }
void CuAsmParser::dispSymtabDict() const { m_impl->dispSymtabDict(); }
void CuAsmParser::dispLabelDict() const { m_impl->dispLabelDict(); }
void CuAsmParser::dispSegmentHeader() const { m_impl->dispSegmentHeader(); }
void CuAsmParser::dispFileHeader() const { m_impl->dispFileHeader(); }
void CuAsmParser::dispTables() const { m_impl->dispTables(); }

std::string CuAsmParser::stripComments(const std::string& s) {
    static const std::regex cpp("//.*");
    static const std::regex c(R"(/\*.*?\*/)");
    static const std::regex bra(R"(\(\*.*\*\))");
    static const std::regex spaces(R"(\s+)");

    std::string r = std::regex_replace(s, cpp, " ");
    r = std::regex_replace(r, c, " ");
    r = std::regex_replace(r, bra, " ");
    r = std::regex_replace(r, spaces, " ");

    return trim(r);
}

} // namespace CuAsm
