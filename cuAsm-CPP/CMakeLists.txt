cmake_minimum_required(VERSION 3.20)

project(cuAsmCPP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(CuAsm)
add_subdirectory(bin)
add_subdirectory(Tests)
