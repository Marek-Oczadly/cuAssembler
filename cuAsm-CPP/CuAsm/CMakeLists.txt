add_subdirectory(utils)

# Sources are added here as CuAsm/*.hpp gain implementations.
add_library(CuAsm STATIC
    common.cpp
    config.cpp
    CuAsmParser.cpp
)

target_include_directories(CuAsm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(CuAsm PUBLIC CuAsmUtils elfio::elfio)
