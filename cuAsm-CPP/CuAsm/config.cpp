#include "config.hpp"

#include <cstring>
#include <filesystem>

namespace CuAsm {

namespace {

namespace fs = std::filesystem;

/**
 * @brief Decodes a hex-digit-pair string into raw bytes.
 * @param hex Hex string, must have an even number of hex-digit characters.
 * @return The decoded bytes.
 **/
std::string hexToBytes(const std::string& hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

/**
 * @brief Gets the directory this source file (and the InsAsmRepos data next to it) resides in,
 *        mirroring how CuAsm.config uses os.path.split(__file__).
 * @return The directory containing config.cpp.
 **/
const fs::path& moduleDir() {
    static const fs::path dir = fs::path(__FILE__).parent_path();
    return dir;
}

} // namespace

const std::string Config::NVDISASM_PATH = "nvdisasm";

const ELFIO::Elf64_Ehdr& Config::defaultCubinFileHeader() {
    static const ELFIO::Elf64_Ehdr header = [] {
        ELFIO::Elf64_Ehdr h{};
        const std::string bytes = hexToBytes(
            "7f454c460201013307000000000000000200be00650000000000000000000000"
            "c09000000000000000890000000000004b054b0040003800030040001f000100");
        std::memcpy(&h, bytes.data(), sizeof(h));
        return h;
    }();
    return header;
}

const ELFIO::Elf64_Shdr& Config::defaultSectionHeader() {
    static const ELFIO::Elf64_Shdr header{};
    return header;
}

const ELFIO::Elf64_Phdr& Config::defaultSegmentHeader() {
    static const ELFIO::Elf64_Phdr header{};
    return header;
}

const ELFIO::Elf64_Sym& Config::defaultSymbol() {
    static const ELFIO::Elf64_Sym symbol{};
    return symbol;
}

const ELFIO::Elf64_Rel& Config::defaultRel() {
    static const ELFIO::Elf64_Rel rel{};
    return rel;
}

const ELFIO::Elf64_Rela& Config::defaultRela() {
    static const ELFIO::Elf64_Rela rela{};
    return rela;
}

void Config::load() {
    // TODO: load from file
}

void Config::save() {
    // TODO: save to file
}

std::string Config::getDefaultInsAsmReposFile(int versionNumber) {
    fs::path reposDir = moduleDir() / "InsAsmRepos";
    fs::path reposPath = reposDir / ("DefaultInsAsmRepos.sm_" + std::to_string(versionNumber) + ".txt");
    return reposPath.string();
}

std::string Config::getDefaultIOInfoFile(int versionNumber) {
    fs::path fdir = moduleDir() / "InsAsmRepos";
    fs::path fpath = fdir / ("IOInfo.sm_" + std::to_string(versionNumber) + ".txt");

    if (!fs::is_regular_file(fpath)) {
        fpath = fdir / "IOInfo.all.json";
    }

    return fpath.string();
}

} // namespace CuAsm
