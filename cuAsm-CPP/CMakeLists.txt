cmake_minimum_required(VERSION 3.20)

project(cuAsmCPP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMAKE_CXX_STANDARD only seeds the CXX_STANDARD target property for CXX sources; it has no
# effect on CUDA_STANDARD (which the two nvcc-compiled microbench targets in
# TestData/microbench/{L2Bank,RegBank} set explicitly to 20, since nvcc's MSVC host-compiler
# pass tops out at C++20). This comment documents that independence -- see those subdirectories'
# CMakeLists.txt for where CUDA_STANDARD is pinned.

# Needed here (not just in Tests/CMakeLists.txt) so ctest run from the top of the build tree
# discovers the tests: CTestTestfile.cmake is only generated at the root if enable_testing() is
# called at or above that root, letting it recurse into the subdirectories that add_test().
enable_testing()

include(FetchContent)
FetchContent_Declare(
    elfio
    GIT_REPOSITORY https://github.com/serge1/ELFIO.git
    GIT_TAG        Release_3.12
)
FetchContent_MakeAvailable(elfio)

# CLI11 is header-only; used by the bin/ CLI tools instead of hand-rolled argv-walking.
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.7.2
)
FetchContent_MakeAvailable(cli11)

# spdlog backs CuAsmLogger's rotating-file/named-logger sink machinery. SPDLOG_USE_STD_FORMAT
# uses C++20 std::format instead of bundling/fetching fmt as a separate dependency, since the
# host build is already C++23.
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
)
FetchContent_MakeAvailable(spdlog)

# Boost.Graph and Boost.Regex are system dependencies (not FetchContent'd, given their size/build
# complexity); install them via your platform's package manager (e.g. vcpkg install boost-graph
# boost-regex, apt install libboost-graph-dev libboost-regex-dev). Rendering also shells out to
# the Graphviz `dot` CLI, which must be on PATH. Boost.Regex (unlike Boost.StringAlgo/
# Boost.Multiprecision) is a compiled library, not header-only, hence needing this instead of
# just `Boost::headers`.
find_package(Boost REQUIRED COMPONENTS graph regex)

add_subdirectory(CuAsm)
add_subdirectory(bin)
add_subdirectory(Tests)
add_subdirectory(TestData)
