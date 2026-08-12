#include "CubinUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <boost/algorithm/string/join.hpp>
#include <elfio/elf_types.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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

/**
 * @brief Throws if the byte range [offset, offset+size) falls outside a buffer of the given
 *        size, mirroring the clean struct.error/ELFError pyelftools raises when a malformed or
 *        truncated cubin's header/section table points past the actual file contents.
 * @param bufSize Size of the buffer being read from.
 * @param offset Start offset of the range to validate.
 * @param size Size of the range to validate.
 * @param what Human-readable description of what is being read, used in the error message.
 */
void checkBinRange(std::size_t bufSize, std::uint64_t offset, std::uint64_t size, const std::string& what) {
    if (offset > bufSize || size > bufSize - offset) {
        throw std::runtime_error("Malformed/truncated cubin: " + what + " (offset=" + std::to_string(offset) +
                                  ", size=" + std::to_string(size) + ") exceeds file size " + std::to_string(bufSize) +
                                  "!");
    }
}

/**
 * @brief Parses a cubin's ELF section header table into memory, validating every offset/size
 *        against the actual buffer length so a malformed/truncated cubin raises a clean
 *        exception instead of reading out of bounds.
 * @param bytes Raw cubin file contents.
 * @param outShstrtab Set to the section header string table's contents.
 * @return The parsed section headers.
 */
std::vector<ELFIO::Elf64_Shdr> parseSectionHeaders(const std::string& bytes, std::string& outShstrtab) {
    checkBinRange(bytes.size(), 0, sizeof(ELFIO::Elf64_Ehdr), "ELF header");
    ELFIO::Elf64_Ehdr ehdr{};
    std::memcpy(&ehdr, bytes.data(), sizeof(ehdr));

    std::vector<ELFIO::Elf64_Shdr> shdrs(ehdr.e_shnum);
    for (std::uint16_t i = 0; i < ehdr.e_shnum; ++i) {
        const std::uint64_t off = ehdr.e_shoff + static_cast<std::uint64_t>(i) * ehdr.e_shentsize;
        checkBinRange(bytes.size(), off, sizeof(ELFIO::Elf64_Shdr), "section header table entry " + std::to_string(i));
        std::memcpy(&shdrs[i], bytes.data() + off, sizeof(ELFIO::Elf64_Shdr));
    }

    if (ehdr.e_shstrndx >= shdrs.size()) {
        throw std::runtime_error("Malformed cubin: section header string table index " +
                                  std::to_string(ehdr.e_shstrndx) + " is out of range!");
    }
    const ELFIO::Elf64_Shdr& shstrtabHdr = shdrs[ehdr.e_shstrndx];
    checkBinRange(bytes.size(), shstrtabHdr.sh_offset, shstrtabHdr.sh_size, "section header string table");
    outShstrtab = bytes.substr(shstrtabHdr.sh_offset, shstrtabHdr.sh_size);

    return shdrs;
}

/** @brief Collects the byte ranges of all ".text.<kernel>" sections in a cubin's bytes. */
std::map<std::string, TextSectionRange> collectTextSections(const std::string& bytes) {
    std::string shstrtab;
    const std::vector<ELFIO::Elf64_Shdr> shdrs = parseSectionHeaders(bytes, shstrtab);

    std::map<std::string, TextSectionRange> secDict;
    for (const ELFIO::Elf64_Shdr& shdr : shdrs) {
        const std::string name = readCString(shstrtab, shdr.sh_name);
        if (name.rfind(".text.", 0) == 0) {
            checkBinRange(bytes.size(), shdr.sh_offset, shdr.sh_size, "section \"" + name + "\"");
            secDict[name] = TextSectionRange{shdr.sh_offset, shdr.sh_size};
        }
    }
    return secDict;
}

/** @brief Sets the desc-show bit on every instruction word within the given byte ranges. */
void setDescBitInRanges(std::string& bytes, const std::map<std::string, TextSectionRange>& secDict) {
    for (const auto& [name, range] : secDict) {
        if (range.size % 16 != 0) {
            throw std::runtime_error("Malformed cubin: text section \"" + name + "\" size " +
                                      std::to_string(range.size) + " is not a multiple of 16!");
        }
        checkBinRange(bytes.size(), range.offset, range.size, "text section \"" + name + "\"");

        for (std::uint64_t off = 0; off < range.size; off += 16) {
            std::uint64_t q2;
            std::memcpy(&q2, bytes.data() + range.offset + off + 8, sizeof(q2));
            q2 |= DESC_BIT;
            std::memcpy(bytes.data() + range.offset + off + 8, &q2, sizeof(q2));
        }
    }
}

/** @brief Reads a whole file's contents into a string, mirroring python's open(path, 'rb').read(). */
std::string readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open file " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/** @brief Strips leading/trailing whitespace, mirroring python's str.strip(). */
std::string trim(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

/** @brief Replaces every occurrence of a literal substring, mirroring python's str.replace(old, new). */
std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return s;
    }
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

/** @brief One ELF section's name and raw contents. */
struct NamedElfSection {
    std::string name;
    std::string data;
};

/** @brief Collects the name and raw data of every ELF section whose name starts with prefix. */
std::vector<NamedElfSection> collectSectionsByPrefix(const std::string& bytes, const std::string& prefix) {
    std::string shstrtab;
    const std::vector<ELFIO::Elf64_Shdr> shdrs = parseSectionHeaders(bytes, shstrtab);

    std::vector<NamedElfSection> result;
    for (const ELFIO::Elf64_Shdr& shdr : shdrs) {
        const std::string name = readCString(shstrtab, shdr.sh_name);
        if (name.rfind(prefix, 0) == 0) {
            checkBinRange(bytes.size(), shdr.sh_offset, shdr.sh_size, "section \"" + name + "\"");
            result.push_back({name, bytes.substr(shdr.sh_offset, shdr.sh_size)});
        }
    }
    return result;
}

/** @brief Formats a CuNVInfoValue the way python's f'{val}' would, for log messages. */
std::string formatNVInfoValueForLog(const CuNVInfoValue& val) {
    if (std::holds_alternative<std::monostate>(val)) {
        return "None";
    }
    if (std::holds_alternative<std::uint32_t>(val)) {
        return std::to_string(std::get<std::uint32_t>(val));
    }
    const auto& vec = std::get<std::vector<std::uint32_t>>(val);
    std::vector<std::string> items;
    items.reserve(vec.size());
    for (std::uint32_t v : vec) {
        items.push_back(std::to_string(v));
    }
    return "[" + boost::algorithm::join(items, ", ") + "]";
}

/** @brief Converts one glob path component ('*'/'?' wildcards) into an equivalent regex. */
std::string globComponentToRegex(const std::string& comp) {
    std::string re;
    for (char c : comp) {
        switch (c) {
            case '*': re += ".*"; break;
            case '?': re += '.'; break;
            case '.': re += "\\."; break;
            case '\\': re += "\\\\"; break;
            case '+': re += "\\+"; break;
            case '^': re += "\\^"; break;
            case '$': re += "\\$"; break;
            case '(': re += "\\("; break;
            case ')': re += "\\)"; break;
            case '[': re += "\\["; break;
            case ']': re += "\\]"; break;
            case '{': re += "\\{"; break;
            case '}': re += "\\}"; break;
            case '|': re += "\\|"; break;
            default: re += c;
        }
    }
    return re;
}

/** @brief Yields file (not directory) paths matching a shell-style glob pattern, mirroring
 *         python's glob.iglob to the extent needed here: wildcards ('*'/'?') are only supported in
 *         the final path component -- directory components of the pattern must be literal. Also
 *         mirrors f_glob's skip-directories behavior. */
std::vector<std::string> globFiles(const std::string& pattern) {
    namespace fs = std::filesystem;
    std::string normalized = pattern;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    const std::size_t slashPos = normalized.find_last_of('/');
    const fs::path dir = (slashPos == std::string::npos) ? fs::path(".") : fs::path(normalized.substr(0, slashPos));
    const std::string filePattern = (slashPos == std::string::npos) ? normalized : normalized.substr(slashPos + 1);

    std::vector<std::string> result;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return result;
    }

    const std::regex re(globComponentToRegex(filePattern));
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (std::regex_match(name, re)) {
            result.push_back((dir / name).string());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

/** @brief Moves a file, falling back to copy+remove across filesystems, mirroring shutil.move. */
void moveFile(const std::string& src, const std::string& dst) {
    std::error_code ec;
    std::filesystem::rename(src, dst, ec);
    if (ec) {
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::remove(src);
    }
}

/** @brief Formats a "idx/total" progress string, mirroring python's f'{idx:4d}/{total}'. */
std::string formatProgress(std::size_t idx, std::size_t total) {
    std::ostringstream oss;
    oss << std::setw(4) << idx << "/" << total;
    return oss.str();
}

/** @brief Builds a per-process temp file name, mirroring CudaBinFile's f'cuasm_{pid:x}.tmp.{suffix}'. */
std::string tempNameForPid(const std::string& suffix) {
    std::ostringstream oss;
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = getpid();
#endif
    oss << "cuasm_" << std::hex << pid << ".tmp." << suffix;
    return oss.str();
}

/// Instruction filter regex excluding lines containing "QNAN", mirroring CubinUtils.f_QNAN
/// (`lambda x: 'QNAN' not in x`) -- CuInsFeeder's insFilter only supports "keep if regex matches",
/// so exclusion is expressed via a negative lookahead spanning the whole line.
const char* const QNAN_EXCLUDE_FILTER = R"(^(?:(?!QNAN).)*$)";

/// Regex mirroring CubinUtils.arch_pattern, used to auto-detect arch from a dumped file's name.
const std::regex& archPatternRe() {
    static const std::regex re(R"(\.(sm_\d+)\.(cubin|ptx)$)");
    return re;
}

/// Regex mirroring CubinUtils.fname_pattern, used to parse `cuobjdump -lelf/-lptx` list lines.
const std::regex& fnamePatternRe() {
    static const std::regex re(R"(file *(\d+): *(.*)$)");
    return re;
}

/** @brief Parses one `cuobjdump -lelf/-lptx` list line, mirroring CubinUtils.parseListLine.
 *  @return The extracted file name, or std::nullopt if the line didn't match. */
std::optional<std::string> parseListLine(const std::string& line) {
    std::smatch m;
    if (!std::regex_search(line, m, fnamePatternRe())) {
        return std::nullopt;
    }
    return trim(m.str(2));
}

} // namespace

const BigInt pShowDesc = BigInt(1) << 101;

void transPTXVersion(const std::string& ptxname, const std::string& outname, const std::string& arch, const std::string& version) {
    static const std::regex targetPattern(R"(^\s*\.target\s+)");
    static const std::regex versionPattern(R"(^\s*\.version\s+)");

    std::ostringstream sio;
    {
        std::ifstream fin(ptxname);
        if (!fin) {
            throw std::runtime_error("Cannot open ptx file " + ptxname);
        }
        std::string line;
        while (std::getline(fin, line)) {
            if (std::regex_search(line, targetPattern)) {
                sio << ".target " << arch << "\n";
            } else if (std::regex_search(line, versionPattern)) {
                sio << ".version " << version << "\n";
            } else {
                sio << line << "\n";
            }
        }
    }

    const std::string resolvedOutname = outname.empty() ? ptxname : outname;
    std::ofstream fout(resolvedOutname);
    fout << sio.str();
}

void feedBinFromCubin(const std::string& binname, const std::function<void(const std::string&)>& callback,
                       const std::string& outname, bool mergeAllKernels) {
    std::string resolvedOutname = outname;
    if (resolvedOutname.empty()) {
        static const std::string cubinSuffix = ".cubin";
        if (binname.size() >= cubinSuffix.size() && binname.compare(binname.size() - cubinSuffix.size(), cubinSuffix.size(), cubinSuffix) == 0) {
            resolvedOutname = replaceAll(binname, cubinSuffix, ".bin");
        } else {
            resolvedOutname = binname + ".bin";
        }
    }

    const std::string fbytes = readFileBytes(binname);
    const std::vector<NamedElfSection> sections = collectSectionsByPrefix(fbytes, ".text");

    if (mergeAllKernels) {
        bool isEmpty = true;
        {
            std::ofstream fout(resolvedOutname, std::ios::binary);
            for (const NamedElfSection& sec : sections) {
                fout.write(sec.data.data(), static_cast<std::streamsize>(sec.data.size()));
                isEmpty = false;
            }
        }
        if (!isEmpty) {
            callback(resolvedOutname);
        }
    } else {
        for (const NamedElfSection& sec : sections) {
            {
                std::ofstream fout(resolvedOutname, std::ios::binary);
                fout.write(sec.data.data(), static_cast<std::streamsize>(sec.data.size()));
            }
            callback(resolvedOutname);
        }
    }
}

std::function<std::optional<InsFeederRecord>()> transDescFeeder(CuInsFeeder& feeder) {
    return [&feeder]() -> std::optional<InsFeederRecord> {
        const auto rec = feeder.next();
        if (!rec) {
            return std::nullopt;
        }
        BigInt code = rec->code;
        if (rec->asmText.find("desc") == std::string::npos) {
            code ^= (code & pShowDesc); // clear the desc-show bit
        }
        return InsFeederRecord{rec->addr, code, rec->asmText, 0};
    };
}

void updateReposWithCubin(CuInsAssemblerRepos& repos, const std::string& binname_, const std::string& savname, bool useNvdisasm,
                          bool doDescHack, bool mergeAllKernels) {
    std::string binname = binname_;
    const std::string arch = repos.getArchString(); // such as "sm_75"
    std::string bArch = arch;
    bArch.erase(std::remove(bArch.begin(), bArch.end(), '_'), bArch.end());
    std::transform(bArch.begin(), bArch.end(), bArch.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    bool useDescTrans = false;
    if (doDescHack && repos.getSMVersion().needsDescHack()) {
        const std::filesystem::path binPath(binname);
        const std::filesystem::path hbinPath = binPath.parent_path() / ("hack." + binPath.filename().string());
        hackCubinDesc(binname, hbinPath.string());
        binname = hbinPath.string();
        useDescTrans = true;
    }

    try {
        if (useNvdisasm) {
            // text sections will be written into a new file and disassembled as binary
            int ncnt = 0;
            feedBinFromCubin(
                binname,
                [&](const std::string& outname) {
                    // NOTE: disassemble from binary will not show fixups, just raw disassembly
                    const std::string sass = checkOutput({Config::NVDISASM_PATH, "-hex", "-c", "-b", bArch, outname});
                    std::istringstream sio(sass);
                    CuInsFeeder feeder(sio, "", QNAN_EXCLUDE_FILTER); // filter QNAN for build

                    if (useDescTrans) {
                        const auto gen = transDescFeeder(feeder);
                        ncnt += repos.update(gen);
                    } else {
                        ncnt += repos.update(feeder);
                    }
                },
                "", mergeAllKernels);

            if (ncnt > 0 && !savname.empty()) { // only save when changed
                repos.save2file(savname);
            }
        } else {
            const std::string sass = checkOutput({"cuobjdump", "-sass", binname});
            std::istringstream sio(sass);
            CuInsFeeder feeder(sio, "", QNAN_EXCLUDE_FILTER); // filter QNAN for build

            if (useDescTrans) {
                const auto gen = transDescFeeder(feeder);
                repos.update(gen);
            } else {
                repos.update(feeder);
            }
        }
    } catch (const CalledProcessError& cpe) {
        CuAsmLogger::logWarning(trim(cpe.output()));
        CuAsmLogger::resetIndent();
        return;
    } catch (const std::exception& e) {
        CuAsmLogger::logError(std::string("Unknown subprocess error (") + e.what() + ")! ");
        CuAsmLogger::resetIndent();
        return;
    }
}

void updateUnknownNVInfoWithCubin(const std::string& binname, std::map<std::string, CuNVInfoValue>& unknownNVInfo) {
    const std::string fbytes = readFileBytes(binname);
    const std::vector<NamedElfSection> sections = collectSectionsByPrefix(fbytes, ".nv.info.");

    for (const NamedElfSection& sec : sections) {
        const std::vector<std::uint8_t> data(sec.data.begin(), sec.data.end());
        CuNVInfo nvinfo(data);

        for (const CuNVInfoAttr& attr : nvinfo.m_AttrList) {
            if (!unknownNVInfo.count(attr.name)) {
                CuAsmLogger::logWarning("Unknown NVInfo Attr (" + attr.name + ") : (" + formatNVInfoValueForLog(attr.value) + ")");
                unknownNVInfo[attr.name] = attr.value;
            }
        }
    }
}

void updateReposWithPTX(CuInsAssemblerRepos& repos, const std::string& ptxname, const std::string& savname) {
    static const std::regex ptxSuffix(R"(\.ptx$)");
    const std::string binname = std::regex_replace(ptxname, ptxSuffix, ".cubin");
    const std::string arch = repos.getArchString();

    try {
        transPTXVersion(ptxname, "", arch);
        checkOutput({"ptxas", "-arch", arch, "-o", binname, ptxname}, /*mergeStderr=*/false);
        updateReposWithCubin(repos, binname, savname);
    } catch (const CalledProcessError& cpe) {
        CuAsmLogger::logWarning(trim(cpe.output()));
        CuAsmLogger::resetIndent();
        return;
    } catch (const std::exception& e) {
        CuAsmLogger::logError(std::string("Unknown subprocess error (") + e.what() + ")! ");
        CuAsmLogger::resetIndent();
        return;
    }
}

CudaBinFile::CudaBinFile(const std::string& name)
    : mName(name), mTempBinName(tempNameForPid("cubin")), mTempPTXName(tempNameForPid("ptx")) {}

void CudaBinFile::resetFileName(const std::string& name) { mName = name; }

void CudaBinFile::dumpFile(const std::string& fname, std::string arch, std::string ftype) {
    if (ftype.empty()) {
        static const std::string cubinSuffix = ".cubin";
        static const std::string ptxSuffix = ".ptx";
        if (fname.size() >= cubinSuffix.size() && fname.compare(fname.size() - cubinSuffix.size(), cubinSuffix.size(), cubinSuffix) == 0) {
            ftype = "elf";
        } else if (fname.size() >= ptxSuffix.size() && fname.compare(fname.size() - ptxSuffix.size(), ptxSuffix.size(), ptxSuffix) == 0) {
            ftype = "ptx";
        } else {
            throw std::runtime_error("Unknown file type for " + fname + "!!!");
        }
    }

    if (arch.empty()) {
        std::smatch m;
        if (std::regex_search(fname, m, archPatternRe())) {
            arch = m.str(1);
        } else {
            throw std::runtime_error("Cannot determine arch for " + fname + "!!!");
        }
    }

    const std::string xopt = "-x" + ftype;
    if (ftype == "elf") {
        checkOutput({"cuobjdump", xopt, fname, "-arch", arch, mName}, /*mergeStderr=*/false);
    } else if (ftype == "ptx") {
        checkOutput({"cuobjdump", xopt, fname, mName}, /*mergeStderr=*/false);
    }
}

std::map<std::string, CuNVInfoValue>& CudaBinFile::updateUnknownNVInfo(const std::string& arch,
                                                                        std::map<std::string, CuNVInfoValue>& unknownNVInfo) {
    const std::vector<std::string> elflist = listFile("elf", arch);
    const std::size_t nelf = elflist.size();
    for (std::size_t ielf = 0; ielf < nelf; ++ielf) {
        const std::string& fname = elflist[ielf];
        try {
            dumpFile(fname, arch);
            moveFile(fname, mTempBinName);

            CuAsmLogger::logProcedure("#### " + formatProgress(ielf, nelf) + " : Updating with " + fname + " ...");
            updateUnknownNVInfoWithCubin(mTempBinName, unknownNVInfo);
        } catch (const CalledProcessError& cpe) {
            CuAsmLogger::logError(cpe.output());
            continue;
        } catch (const std::exception& e) {
            CuAsmLogger::logError(e.what());
            continue;
        }
    }

    return unknownNVInfo;
}

std::vector<std::string> CudaBinFile::iterELF(const std::string& arch) { return listFile("elf", arch); }

std::vector<std::string> CudaBinFile::iterPTX() { return listFile("ptx", ""); }

void CudaBinFile::updateRepos(CuInsAssemblerRepos& repos, const std::string& savename, const std::set<std::string>& ftype) {
    std::string arch = repos.getArchString();
    std::transform(arch.begin(), arch.end(), arch.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ftype.count("elf")) {
        const std::vector<std::string> elflist = listFile("elf", arch);
        const std::size_t nelf = elflist.size();
        for (std::size_t ielf = 0; ielf < nelf; ++ielf) {
            const std::string& fname = elflist[ielf];
            try {
                dumpFile(fname, arch);
                moveFile(fname, mTempBinName);
                CuAsmLogger::logProcedure("#### " + formatProgress(ielf, nelf) + " : Updating with " + fname + " ...");
                updateReposWithCubin(repos, mTempBinName, savename);
            } catch (const CalledProcessError& cpe) {
                CuAsmLogger::logError(cpe.output());
                continue;
            } catch (const std::exception& e) {
                CuAsmLogger::logError(e.what());
                continue;
            }
        }
    }

    if (ftype.count("ptx")) {
        const std::vector<std::string> ptxlist = listFile("ptx", "");
        const std::size_t nptx = ptxlist.size();
        for (std::size_t iptx = 0; iptx < nptx; ++iptx) {
            const std::string& fname = ptxlist[iptx];
            try {
                dumpFile(fname, arch);
                moveFile(fname, mTempPTXName);

                transPTXVersion(mTempPTXName, "", arch);

                CuAsmLogger::logProcedure("#### " + formatProgress(iptx, nptx) + " : Updating with " + fname + " ...");
                updateReposWithPTX(repos, mTempPTXName, savename);
            } catch (const CalledProcessError& cpe) {
                CuAsmLogger::logError(cpe.output());
                continue;
            } catch (const std::exception& e) {
                CuAsmLogger::logError(e.what());
                continue;
            }
        }
    }
}

std::vector<std::string> CudaBinFile::listFile(const std::string& ftype, const std::string& arch) {
    const std::string lopt = "-l" + ftype;
    std::string outlist;
    try {
        if (ftype == "elf") {
            outlist = checkOutput({"cuobjdump", lopt, "-arch", arch, mName}, /*mergeStderr=*/false);
        } else if (ftype == "ptx") { // no arch specifier for ptx, may be modified
            outlist = checkOutput({"cuobjdump", lopt, mName}, /*mergeStderr=*/false);
        }
    } catch (const CalledProcessError& cpe) {
        CuAsmLogger::logError(cpe.output());
        return {};
    } catch (const std::exception& e) {
        CuAsmLogger::logError(e.what());
        return {};
    }

    std::vector<std::string> flist;
    std::istringstream iss(outlist);
    std::string line;
    while (std::getline(iss, line)) {
        if (const auto fname = parseListLine(line)) {
            flist.push_back(*fname);
        }
    }
    return flist;
}

void CudaBinFile::iterProcess(const std::function<void(const std::string&)>& callback, const std::string& arch, const std::string& ftype) {
    std::string tname;
    if (ftype == "elf") {
        tname = mTempBinName;
    } else if (ftype == "ptx") {
        tname = mTempPTXName;
    } else {
        throw std::runtime_error("Unknown ftype " + ftype + "!!!");
    }

    const std::vector<std::string> flist = listFile(ftype, arch);
    const std::size_t nfile = flist.size();
    for (std::size_t i = 0; i < nfile; ++i) {
        const std::string& fname = flist[i];
        try {
            dumpFile(fname, arch, ftype);
            moveFile(fname, tname);

            CuAsmLogger::logProcedure("#### " + formatProgress(i, nfile) + " : Updating with " + fname + " ...");
            callback(tname);
        } catch (const CalledProcessError& cpe) {
            CuAsmLogger::logError(cpe.output());
            continue;
        } catch (const std::exception& e) {
            CuAsmLogger::logError(e.what());
            continue;
        }
    }
}

void updateNVInfoForArch(const std::vector<std::string>& fpatternList, const std::string& arch) {
    std::map<std::string, CuNVInfoValue> unknownNVInfo;

    for (const std::string& fpattern : fpatternList) {
        for (const std::string& fname : globFiles(fpattern)) {
            CuAsmLogger::logEntry("Processing dll file " + fname);
            try {
                CudaBinFile cbf(fname);
                cbf.updateUnknownNVInfo(arch, unknownNVInfo);
            } catch (const std::exception& e) {
                CuAsmLogger::logError(e.what());
                continue;
            }
        }
    }
}

void iterProcessFilesFromBinFiles(const std::vector<std::string>& fpatternList, const std::string& arch,
                                   const std::function<void(const std::string&)>& callback, const std::string& ftype,
                                   const std::function<void()>& callback2, const std::function<void()>& callback3) {
    for (const std::string& fpattern : fpatternList) {
        for (const std::string& fname : globFiles(fpattern)) {
            CuAsmLogger::logEntry("#### Processing file " + fname);
            try {
                CudaBinFile cbf(fname);
                cbf.iterProcess(callback, arch, ftype);
                if (callback2) {
                    callback2();
                }
            } catch (const std::exception& e) {
                CuAsmLogger::logError(e.what());
                continue;
            }
        }

        if (callback3) {
            callback3();
        }
    }
}

bool hackCubinDesc(const std::string& fin, const std::string& fout, bool alwaysOutput) {
    return CuAsmLogger::logTimeIt("hackCubinDesc", [&]() -> bool {
        const std::string fbytes = readFileBytes(fin);
        if (fbytes.size() < sizeof(ELFIO::Elf64_Ehdr)) {
            throw std::runtime_error("Cubin file " + fin + " is too small to contain an ELF header!");
        }

        ELFIO::Elf64_Ehdr ehdr{};
        std::memcpy(&ehdr, fbytes.data(), sizeof(ehdr));

        const int smv = static_cast<int>(ehdr.e_flags & 0xff) / 10; // SM major version
        if (smv < 8) {                                              // pre-Ampere, no need to hack
            if (alwaysOutput) {
                std::filesystem::copy_file(fin, fout, std::filesystem::copy_options::overwrite_existing);
            }
            return false;
        }

        const std::map<std::string, TextSectionRange> secDict = collectTextSections(fbytes);

        std::string hackedBytes = fbytes;
        setDescBitInRanges(hackedBytes, secDict);

        std::ofstream foutStream(fout, std::ios::binary);
        foutStream.write(hackedBytes.data(), static_cast<std::streamsize>(hackedBytes.size()));

        return true;
    })();
}

// @CuAsmLogger.logTimeIt
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
    // secDict.at() throws std::out_of_range for a kernel with no matching .text section, mirroring
    // python's sec_dict[secname] raising KeyError rather than silently skipping the kernel.
    std::string outBytes = fbytes;
    for (const auto& [kname, addrList] : descDict) {
        const TextSectionRange& range = secDict.at(".text." + kname);
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

bool demoteDefaultDescBits(const std::string& cubinPath) {
    std::string fbytes = readFileBytes(cubinPath);
    if (fbytes.size() < sizeof(ELFIO::Elf64_Ehdr)) {
        throw std::runtime_error("Cubin file " + cubinPath + " is too small to contain an ELF header!");
    }

    ELFIO::Elf64_Ehdr ehdr{};
    std::memcpy(&ehdr, fbytes.data(), sizeof(ehdr));
    const int smv = static_cast<int>(ehdr.e_flags & 0xff) / 10;
    if (smv < 8) {
        return false;
    }

    const std::map<std::string, TextSectionRange> secDict = collectTextSections(fbytes);

    // Disassemble the cubin exactly as CuAsmParser produced it (no bit-forcing needed this time --
    // every instruction that shows desc[URx] here already has DESC_BIT=1 for real).
    const std::string sass = checkOutput({Config::NVDISASM_PATH, "-hex", "-c", cubinPath});

    // Per kernel, collect (address, descriptor UR number) for every instruction that shows desc[].
    static const std::regex kDescRe(R"(desc\[UR(\d+)\])");
    std::map<std::string, std::vector<std::pair<std::uint64_t, int>>> descByFunc;
    {
        std::istringstream sassStream(sass);
        CuInsFeeder feeder(sassStream);
        while (const auto rec = feeder.next()) {
            std::smatch m;
            if (std::regex_search(rec->asmText, m, kDescRe)) {
                descByFunc[feeder.CurrFuncName].emplace_back(rec->addr, std::stoi(m[1].str()));
            }
        }
    }

    // Within a kernel, an explicit desc[URx] that CuAsmParser encoded from cuasm source is
    // frequently just a compiler-side artifact of always routing through the same "default"
    // cache-policy UR pair -- nvdisasm has no way to tell that case apart from a genuinely
    // explicit, kernel-specific descriptor at the point the .cuasm was written (README.md's
    // "hcubin" section documents the same ambiguity for the disassembly direction). Empirically,
    // the UR used for the implicit default is whichever one is the STRICT MAJORITY of desc[]
    // hits in that kernel; treat that one as spurious and clear DESC_BIT for it, leaving any
    // minority (genuinely explicit) UR references untouched. Skip kernels with no strict majority
    // (e.g. an even split) rather than risk clearing a real explicit reference.
    std::string outBytes = fbytes;
    bool changed = false;
    for (const auto& [kname, hits] : descByFunc) {
        std::map<int, std::vector<std::uint64_t>> byUR;
        for (const auto& [addr, ur] : hits) {
            byUR[ur].push_back(addr);
        }

        int majorityUR = -1;
        std::size_t majorityCount = 0;
        for (const auto& [ur, addrs] : byUR) {
            if (addrs.size() > majorityCount) {
                majorityCount = addrs.size();
                majorityUR = ur;
            }
        }
        if (majorityUR < 0 || majorityCount * 2 <= hits.size()) {
            continue; // no strict majority -- leave this kernel's desc bits as CuAsmParser encoded them
        }

        const TextSectionRange& range = secDict.at(".text." + kname);
        for (const std::uint64_t addr : byUR.at(majorityUR)) {
            const std::uint64_t offset = range.offset + addr + 8;
            std::uint64_t q2;
            std::memcpy(&q2, outBytes.data() + offset, sizeof(q2));
            q2 &= ~DESC_BIT;
            std::memcpy(outBytes.data() + offset, &q2, sizeof(q2));
            changed = true;
        }
    }

    if (changed) {
        CuAsmLogger::logProcedure("Demoting default-UR desc bits in " + cubinPath + " ...");
        std::ofstream foutStream(cubinPath, std::ios::binary);
        foutStream.write(outBytes.data(), static_cast<std::streamsize>(outBytes.size()));
    }

    return changed;
}

} // namespace CuAsm
