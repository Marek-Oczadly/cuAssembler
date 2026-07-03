#pragma once

#include <string>

namespace CuAsm {

class CuAsmParser {
public:
    // Parses a .cuasm source file, populating internal state for assembly.
    void parse(const std::string& asmFileName);

    // Assembles the previously parsed source and writes it out as a .cubin file.
    void saveAsCubin(const std::string& binFileName);
};

} // namespace CuAsm
