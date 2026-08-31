# Header-only for now; sources will be added as utils/*.hpp gain implementations.
add_library(CuAsmUtils INTERFACE)

target_include_directories(CuAsmUtils INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
