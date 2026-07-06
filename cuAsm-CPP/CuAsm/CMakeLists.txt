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
    utils/CubinUtils.cpp
)

target_include_directories(CuAsm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Boost.Multiprecision (used by CuInsAssembler for exact-rational linear algebra) is header-only
# and part of the same Boost distribution found via find_package(Boost) in the top-level
# CMakeLists.txt, so no extra COMPONENTS/dependency is needed beyond linking its headers target.
target_link_libraries(CuAsm PUBLIC CuAsmUtils elfio::elfio Boost::headers)
