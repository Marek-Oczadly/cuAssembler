add_subdirectory(utils)

# Sources are added here as CuAsm/*.hpp gain implementations.
add_library(CuAsm STATIC
    common.cpp
    config.cpp
    CuAsmParser.cpp
    CubinFile.cpp
    CuControlCode.cpp
    CuInsAssembler.cpp
    CuInsAssemblerRepos.cpp
    CuInsFeeder.cpp
    CuInsParser.cpp
    CuKernelAssembler.cpp
    CuNVInfo.cpp
    CuSMVersion.cpp
    LatencyClass.cpp
    OperandRole.cpp
    utils/CubinUtils.cpp
)

target_include_directories(CuAsm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    # include/: CuAsmTools/*.hpp, the clean C++ interface to the bin/ CLI tools (see
    # include/CuAsmTools/CuAsmTools.hpp) -- PUBLIC here so every consumer of CuAsm can
    # `#include "CuAsmTools/VerifyCC.hpp"` etc. without a relative "../../include/..." path.
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# Boost.Multiprecision (used by CuInsAssembler for exact-rational linear algebra) is header-only,
# so no extra COMPONENTS/dependency is needed beyond linking its headers target. Boost.Regex
# (used by CuInsParser for lookbehind-dependent patterns std::regex can't express) is a compiled
# library, hence the separate `regex` COMPONENTS entry in the top-level find_package(Boost) call.
# spdlog is PUBLIC since CuAsmLogger.hpp (which wraps it) is itself a public header included
# directly by bin/ tools, not just other CuAsm sources.
target_link_libraries(CuAsm PUBLIC CuAsmUtils elfio::elfio Boost::headers Boost::regex spdlog::spdlog)
