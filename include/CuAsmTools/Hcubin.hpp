#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "../../CuAsm/utils/CubinUtils.hpp"

// Clean C++ entry point for the `hcubin` CLI tool (bin/hcubin.cpp) -- a thin, default-output-
// path-resolving wrapper around the already-public CuAsm::fixCubinDesc(). Unlike the CLI, this
// throws on failure instead of logging and returning std::nullopt, never touches CuAsmLogger/
// std::cout, and always overwrites an existing output path (see CuAsmTools/Cuasm.hpp's header doc
// for why the CLI's backup-existing-output-file behavior isn't replicated here).

namespace CuAsm::Tools {

/**
 * @brief Hacks a cubin's SM8x cache-policy desc bit so it is shown explicitly wherever needed,
 *        writing the result to a new cubin file, mirroring bin/hcubin.cpp's own hcubin() local
 *        function.
 * @param cubinPath Path of the input cubin file.
 * @param outPath Optional output path; defaults to cubinPath with its extension replaced by ".hcubin".
 * @return True if the cubin needed (and received) the hack, with the result written to outPath;
 *         false if its SM version predates Ampere and no hack was necessary, in which case
 *         outPath is NOT written at all (matching CuAsm::fixCubinDesc()'s own contract).
 * @throws std::exception on any IO/parse failure (see CuAsm::fixCubinDesc()).
 **/
inline bool hackCubinCachePolicyDesc(const std::string& cubinPath, std::optional<std::string> outPath = std::nullopt) {
    const std::string resolvedOut = outPath.value_or(std::filesystem::path(cubinPath).replace_extension(".hcubin").string());
    return fixCubinDesc(cubinPath, resolvedOut);
}

} // namespace CuAsm::Tools
