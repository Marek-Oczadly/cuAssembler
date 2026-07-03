add_subdirectory(utils)

# Header-only for now; sources will be added as CuAsm/*.hpp gain implementations.
add_library(CuAsm INTERFACE)

target_include_directories(CuAsm INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(CuAsm INTERFACE CuAsmUtils)
