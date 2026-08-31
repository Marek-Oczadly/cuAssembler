#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include "../../CuAsm/CuAsmParser.hpp"
#include "../../CuAsm/CubinFile.hpp"

// Clean C++ entry point for the `cuasm` CLI tool's cubin<->cuasm conversion (bin/cuasm.cpp), so
// embedding code -- e.g. the SASS instruction-shuffling project this repo supports -- can perform
// the same conversion without shelling out to the executable. Unlike the CLI:
//   - these functions throw on failure instead of printing a diagnostic and returning false/-1;
//   - they never touch std::cout/CuAsmLogger (the CLI logs/prints around a call to these);
//   - they always overwrite an existing output path rather than renaming it to "path~" first --
//     that backup-on-overwrite behavior is bin/cliCommon.hpp's checkOutFileBackup(), an
//     interactive-CLI safety net, not something a programmatic caller managing its own filesystem
//     state should have applied silently underneath it. bin/cuasm.cpp still calls
//     checkOutFileBackup() itself before invoking convert()/disassembleCubin()/assembleCuasm(),
//     so the CLI's behavior is unchanged.

namespace CuAsm::Tools {

/// Conversion direction for convert(), mirroring bin/cuasm.cpp's --bin2asm/--asm2bin flags.
enum class CuasmDirection {
    Auto,    ///< Infer from `inPath`'s extension (.cubin/.bin -> disassemble, .cuasm/.asm -> assemble).
    Bin2Asm, ///< Force cubin -> cuasm (disassembly).
    Asm2Bin, ///< Force cuasm -> cubin (assembly).
};

/**
 * @brief Disassembles a cubin file to a cuasm text file.
 * @param cubinPath Path of the input cubin file.
 * @param cuasmPath Output cuasm path; defaults to cubinPath with its extension replaced by ".cuasm".
 * @return The resolved output path that was written.
 * @throws std::exception on any parse/IO failure (see CubinFile's constructor / saveAsCuAsm()).
 **/
inline std::string disassembleCubin(const std::string& cubinPath, std::optional<std::string> cuasmPath = std::nullopt) {
    const std::string outPath = cuasmPath.value_or(std::filesystem::path(cubinPath).replace_extension(".cuasm").string());
    CubinFile cf(cubinPath);
    cf.saveAsCuAsm(outPath);
    return outPath;
}

/**
 * @brief Assembles a cuasm text file to a cubin file.
 * @param cuasmPath Path of the input cuasm file.
 * @param cubinPath Output cubin path; defaults to cuasmPath with its extension replaced by ".cubin".
 * @return The resolved output path that was written.
 * @throws std::exception on any parse/IO failure (see CuAsmParser::parse() / saveAsCubin()).
 **/
inline std::string assembleCuasm(const std::string& cuasmPath, std::optional<std::string> cubinPath = std::nullopt) {
    CuAsmParser cap;
    cap.parse(cuasmPath);
    const std::string outPath = cubinPath.value_or(std::filesystem::path(cuasmPath).replace_extension(".cubin").string());
    cap.saveAsCubin(outPath);
    return outPath;
}

/**
 * @brief Converts between cubin and cuasm, choosing direction from `inPath`'s extension unless
 *        overridden, mirroring bin/cuasm.cpp's doProcess().
 * @param inPath Source file (.cubin/.bin or .cuasm/.asm, unless direction forces one way).
 * @param outPath Optional destination path; inferred from inPath if not given.
 * @param direction Conversion direction (default: infer from extension).
 * @return The resolved output path that was written.
 * @throws std::invalid_argument if direction is Auto and inPath's extension is neither a
 *         recognized cubin nor cuasm extension.
 * @throws std::exception on any underlying parse/IO failure.
 **/
inline std::string convert(const std::string& inPath, std::optional<std::string> outPath = std::nullopt,
                            CuasmDirection direction = CuasmDirection::Auto) {
    const std::string ext = std::filesystem::path(inPath).extension().string();

    if (direction == CuasmDirection::Bin2Asm || ext == ".cubin" || ext == ".bin") {
        return disassembleCubin(inPath, std::move(outPath));
    } else if (direction == CuasmDirection::Asm2Bin || ext == ".cuasm" || ext == ".asm") {
        return assembleCuasm(inPath, std::move(outPath));
    }
    throw std::invalid_argument("convert: cannot infer direction for \"" + inPath +
                                 "\" -- pass CuasmDirection::Bin2Asm/Asm2Bin explicitly");
}

} // namespace CuAsm::Tools
