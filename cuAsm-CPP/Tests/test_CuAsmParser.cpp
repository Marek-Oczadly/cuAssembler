#include <string>

#include "../CuAsm/CuAsmParser.hpp"

/**
 * @brief Parses a .cuasm file, reassembles it as a .cubin, and compares the result against a
 *        reference .cubin, mirroring the original Tests/test_CuAsmParser.py script.
 * @return 0 on success.
 **/
int main() {
    // std::string fprefix = "G:\\Work\\CuAssembler\\TestData\\CuTest\\cudatest.7.sm_75";
    std::string fprefix = "G:\\Work\\CuAssembler\\TestData\\CuTest\\cudatest.6.sm_61";

    std::string fname = fprefix + ".cuasm";
    std::string bname = fprefix + ".cubin";
    std::string sname = fprefix + "_new.cubin";

    CuAsm::CuAsmParser cap;
    // cap.setInsAsmRepos("G:\\work\\tmp.sm_61.txt", "sm_61");
    cap.parse(fname);

    // cap.dispRelocationList();
    // cap.dispFileHeader();
    // cap.dispSymbolDict();
    // cap.dispSymtabDict();
    // cap.dispSegmentHeader();
    // cap.dispTables();

    // cap.dispFixupList();
    // cap.dispFixupList();
    // cap.dispRelocationList();
    // cap.dispLabelDict();

    cap.saveAsCubin(sname);
    cap.saveCubinCmp(bname, fprefix);

    cap.dispSectionList();

    return 0;
}
