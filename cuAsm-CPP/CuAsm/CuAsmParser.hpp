#pragma once

#include <string>

namespace CuAsm {

class CuAsmParser {
public:
    // Parses a .cuasm source file, populating internal state for assembly.
    void parse(const std::string& asmFileName);

    // Assembles the previously parsed source and writes it out as a .cubin file.
    void saveAsCubin(const std::string& binFileName);

    // Prints the currently assembled cubin's section list (offset/size/type/flags/name) to stdout.
    void dispSectionList();

    // Debug helper: writes the currently assembled contents and the given reference cubin's
    // contents (file header, section headers/data) side-by-side to "<savPrefix>_asm.txt" and
    // "<savPrefix>_bin.txt" for comparison.
    void saveCubinCmp(const std::string& cubinName, const std::string& savPrefix);
};

} // namespace CuAsm
