#include "CubinUtils.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include <elfio/elf_types.hpp>

#include "../CuAsmLogger.hpp"
#include "../CuInsFeeder.hpp"
#include "../common.hpp"
#include "../config.hpp"

namespace CuAsm {

namespace {

/// Bit within the second 8-byte word of a 16-byte instruction that shows an explicit desc[] on SM8x.
constexpr std::uint64_t DESC_BIT = std::uint64_t(1) << 37;

/** @brief Byte range (offset, size) of a ".text.<kernel>" section within the cubin. */
struct TextSectionRange {
    std::uint64_t offset;
    std::uint64_t size;
};

/** @brief Reads a NUL-terminated string out of a string table at a given byte offset. */
std::string readCString(const std::string& strtab, std::uint32_t offset) {
    if (offset >= strtab.size()) {
        return "";
    }
    const std::size_t end = strtab.find('\0', offset);
    return strtab.substr(offset, (end == std::string::npos ? strtab.size() : end) - offset);
}

/** @brief Collects the byte ranges of all ".text.<kernel>" sections in a cubin's bytes. */
std::map<std::string, TextSectionRange> collectTextSections(const std::string& bytes) {
    ELFIO::Elf64_Ehdr ehdr{};
    std::memcpy(&ehdr, bytes.data(), sizeof(ehdr));

    std::vector<ELFIO::Elf64_Shdr> shdrs(ehdr.e_shnum);
    for (std::uint16_t i = 0; i < ehdr.e_shnum; ++i) {
        const std::size_t off = ehdr.e_shoff + static_cast<std::size_t>(i) * ehdr.e_shentsize;
        std::memcpy(&shdrs[i], bytes.data() + off, sizeof(ELFIO::Elf64_Shdr));
    }

    const ELFIO::Elf64_Shdr& shstrtabHdr = shdrs[ehdr.e_shstrndx];
    const std::string shstrtab = bytes.substr(shstrtabHdr.sh_offset, shstrtabHdr.sh_size);

    std::map<std::string, TextSectionRange> secDict;
    for (const ELFIO::Elf64_Shdr& shdr : shdrs) {
        const std::string name = readCString(shstrtab, shdr.sh_name);
        if (name.rfind(".text.", 0) == 0) {
            secDict[name] = TextSectionRange{shdr.sh_offset, shdr.sh_size};
        }
    }
    return secDict;
}

/** @brief Sets the desc-show bit on every instruction word within the given byte ranges. */
void setDescBitInRanges(std::string& bytes, const std::map<std::string, TextSectionRange>& secDict) {
    for (const auto& [name, range] : secDict) {
        for (std::uint64_t off = 0; off < range.size; off += 16) {
            std::uint64_t q2;
            std::memcpy(&q2, bytes.data() + range.offset + off + 8, sizeof(q2));
            q2 |= DESC_BIT;
            std::memcpy(bytes.data() + range.offset + off + 8, &q2, sizeof(q2));
        }
    }
}

} // namespace

bool fixCubinDesc(const std::string& fin, const std::string& fout) {
    std::string fbytes;
    {
        std::ifstream finStream(fin, std::ios::binary);
        if (!finStream) {
            throw std::runtime_error("Cannot open cubin file " + fin);
        }
        std::ostringstream ss;
        ss << finStream.rdbuf();
        fbytes = ss.str();
    }

    if (fbytes.size() < sizeof(ELFIO::Elf64_Ehdr)) {
        throw std::runtime_error("Cubin file " + fin + " is too small to contain an ELF header!");
    }

    ELFIO::Elf64_Ehdr ehdr{};
    std::memcpy(&ehdr, fbytes.data(), sizeof(ehdr));

    const int smv = static_cast<int>(ehdr.e_flags & 0xff) / 10;
    if (smv < 8) { // SM major version < 8 means pre-Ampere, no need to hack/fix
        CuAsmLogger::logProcedure("Cubin (" + fin + ") with SM major version " + std::to_string(smv) +
                                   " does not need desc hack! Skipping...");
        return false;
    }

    CuAsmLogger::logProcedure("Hacking cubin bytes from " + fin + " ...");
    const std::map<std::string, TextSectionRange> secDict = collectTextSections(fbytes);

    // First set every instruction's desc bit to 1, so nvdisasm shows desc[] explicitly everywhere.
    std::string hackedBytes = fbytes;
    setDescBitInRanges(hackedBytes, secDict);

    const std::string tmpName = getTempFileName("", "cuasm", "cubin");
    CuAsmLogger::logProcedure("Writing hacked cubin to " + tmpName + " ...");
    {
        std::ofstream tmpOut(tmpName, std::ios::binary);
        tmpOut.write(hackedBytes.data(), static_cast<std::streamsize>(hackedBytes.size()));
    }

    const std::string sass = checkOutput({Config::NVDISASM_PATH, "-hex", "-c", tmpName});
    std::filesystem::remove(tmpName);

    // Locate all instructions whose disassembly actually shows a desc[] operand.
    CuAsmLogger::logProcedure("Locating valid desc instructions ...");
    std::map<std::string, std::vector<std::uint64_t>> descDict;
    {
        std::istringstream sassStream(sass);
        CuInsFeeder feeder(sassStream);
        while (const auto rec = feeder.next()) {
            if (rec->asmText.find("desc[") != std::string::npos) {
                descDict[feeder.CurrFuncName].push_back(rec->addr);
            }
        }
    }

    // Set the desc bit only on those instructions, starting from the original (unmodified) bytes.
    std::string outBytes = fbytes;
    for (const auto& [kname, addrList] : descDict) {
        const auto it = secDict.find(".text." + kname);
        if (it == secDict.end()) {
            continue;
        }
        const TextSectionRange& range = it->second;
        for (const std::uint64_t addr : addrList) {
            const std::uint64_t offset = range.offset + addr + 8;
            std::uint64_t q2;
            std::memcpy(&q2, outBytes.data() + offset, sizeof(q2));
            q2 |= DESC_BIT;
            std::memcpy(outBytes.data() + offset, &q2, sizeof(q2));
        }
    }

    CuAsmLogger::logProcedure("Writing fixed cubin to " + fout + " ...");
    std::ofstream foutStream(fout, std::ios::binary);
    foutStream.write(outBytes.data(), static_cast<std::streamsize>(outBytes.size()));

    return true;
}

} // namespace CuAsm
