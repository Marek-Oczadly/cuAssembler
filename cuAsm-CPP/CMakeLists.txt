cmake_minimum_required(VERSION 3.20)

project(cuAsmCPP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
