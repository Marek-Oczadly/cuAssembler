#pragma once

#include <string>

namespace CuAsm {

/// Pseudo section name used to track the ELF program-header table's own range during
/// section/segment layout, mirroring CubinFile.PROGRAM_HEADER_TAG.
inline constexpr const char* PROGRAM_HEADER_TAG = "@PROGRAM_HEADER";

class CubinFile {
public:
    explicit CubinFile(const std::string& binFileName);

    // Disassembles the loaded cubin and writes the result as a .cuasm file.
    void saveAsCuAsm(const std::string& asmFileName);
};

} // namespace CuAsm
