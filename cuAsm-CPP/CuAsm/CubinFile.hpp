#pragma once

#include <string>

namespace CuAsm {

class CubinFile {
public:
    explicit CubinFile(const std::string& binFileName);

    // Disassembles the loaded cubin and writes the result as a .cuasm file.
    void saveAsCuAsm(const std::string& asmFileName);
};

} // namespace CuAsm
