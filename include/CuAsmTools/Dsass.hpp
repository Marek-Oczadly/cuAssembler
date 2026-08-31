#pragma once

#include <cctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "../../CuAsm/CuInsFeeder.hpp"
#include "../../CuAsm/common.hpp"
#include "../../CuAsm/utils/CubinUtils.hpp"

// Clean C++ entry point for the `dsass` CLI tool (bin/dsass.cpp): dumping sass annotated with
// scoreboard control codes from a dumped-sass/cubin/arbitrary-binary file. See bin/dsass.cpp's
// top-of-file doc for the .sass/.cubin/other-extension dispatch this mirrors, including the SM8x
// cache-policy desc-bit hack applied to cubins by default. Unlike the CLI, this throws on failure
// instead of logging and returning false, never touches CuAsmLogger/std::cout, and always
// overwrites an existing output path (see CuAsmTools/Cuasm.hpp's header doc for why the CLI's
// backup-existing-output-file behavior isn't replicated here).

namespace CuAsm::Tools {

/**
 * @brief Dumps sass (from a dumped-sass/cubin/arbitrary-binary file) annotated with scoreboard
 *        control codes, mirroring bin/dsass.cpp's own dsass() local function.
 * @param inPath Input file: dumped sass (.sass), cubin (.cubin), or any other binary containing one.
 * @param outPath Optional output path; defaults to inPath with its extension replaced by ".dsass".
 * @param keepCodeOnlyLines Keep code-only lines in the output instead of stripping them (-k/--keepcode).
 * @param skipDescHack Skip the SM8x cache-policy desc-bit hack applied to a .cubin input regardless
 *        of its SM version (-n/--nodeschack); has no effect on non-.cubin inputs, which never get
 *        the hack (matching bin/dsass.cpp).
 * @return The resolved output path that was written.
 * @throws std::invalid_argument if inPath is already a .dsass file.
 * @throws CuAsm::CalledProcessError if cuobjdump fails to run or exits non-zero.
 * @throws std::exception on any other IO/translation failure.
 **/
inline std::string dumpControlCodeSass(const std::string& inPath, std::optional<std::string> outPath = std::nullopt,
                                        bool keepCodeOnlyLines = false, bool skipDescHack = false) {
    namespace fs = std::filesystem;

    const fs::path inFsPath(inPath);
    std::string ext = inFsPath.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == ".dsass") {
        throw std::invalid_argument("dumpControlCodeSass: input \"" + inPath + "\" is already a dsass file");
    }

    const std::string outName = outPath.value_or(fs::path(inFsPath).replace_extension(".dsass").string());
    const std::string codeOnlyLineMode = keepCodeOnlyLines ? "keep" : "none";

    if (ext == ".sass") {
        CuInsFeeder feeder(inPath);
        feeder.trans(outName, codeOnlyLineMode);
        return outName;
    }

    // .cubin (with the optional desc hack) and every other extension (an arbitrary binary assumed
    // to contain a cubin, e.g. a .exe -- never desc-hacked, matching bin/dsass.cpp's "else" branch)
    // both dump via cuobjdump and feed the result through the same CuInsFeeder translation.
    std::string binname = inPath;
    std::string tmpname;
    bool doDescHack = false;
    if (ext == ".cubin" && !skipDescHack) {
        tmpname = getTempFileName("", "cuasm", "cubin");
        doDescHack = fixCubinDesc(inPath, tmpname);
        if (doDescHack) {
            binname = tmpname;
        }
    }

    const std::string sass = checkOutput({"cuobjdump", "-sass", binname}, /*mergeStderr=*/false);
    if (doDescHack) {
        fs::remove(tmpname);
    }

    std::istringstream sio(sass);
    CuInsFeeder feeder(sio);
    feeder.trans(outName, codeOnlyLineMode);
    return outName;
}

} // namespace CuAsm::Tools
