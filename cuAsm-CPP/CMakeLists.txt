cmake_minimum_required(VERSION 3.20)

project(cuAsmCPP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# Boost.Graph is a system dependency (not FetchContent'd, given its size/build complexity);
# install it via your platform's package manager (e.g. vcpkg install boost-graph, apt install
# libboost-graph-dev). Rendering also shells out to the Graphviz `dot` CLI, which must be on PATH.
find_package(Boost REQUIRED COMPONENTS graph)

add_subdirectory(CuAsm)
add_subdirectory(bin)
add_subdirectory(Tests)
add_subdirectory(TestData)
