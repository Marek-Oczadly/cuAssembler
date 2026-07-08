enable_testing()

# Resolved at configure time, so it always points at TestData/ under wherever
# this repo happens to be checked out, instead of the original scripts'
# hardcoded dev-machine paths (G:\Repos\..., G:\Work\...).
add_compile_definitions(CUASM_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../TestData")

add_executable(test_CuAsmParser test_CuAsmParser.cpp)
target_link_libraries(test_CuAsmParser PRIVATE CuAsm)
add_test(NAME test_CuAsmParser COMMAND test_CuAsmParser)

add_executable(test_CuInsParser test_CuInsParser.cpp)
target_link_libraries(test_CuInsParser PRIVATE CuAsm)
add_test(NAME test_CuInsParser COMMAND test_CuInsParser)

add_executable(test_CubinFile test_CubinFile.cpp)
target_link_libraries(test_CubinFile PRIVATE CuAsm)
add_test(NAME test_CubinFile COMMAND test_CubinFile)

add_executable(test_elfSection test_elfSection.cpp)
target_link_libraries(test_elfSection PRIVATE CuAsm elfio::elfio)
add_test(NAME test_elfSection COMMAND test_elfSection)

add_executable(test_nvinfo test_nvinfo.cpp)
target_link_libraries(test_nvinfo PRIVATE CuAsm elfio::elfio Boost::graph)
add_test(NAME test_nvinfo COMMAND test_nvinfo)

add_executable(test_relocation test_relocation.cpp)
target_link_libraries(test_relocation PRIVATE CuAsm elfio::elfio)
add_test(NAME test_relocation COMMAND test_relocation)

add_subdirectory(CheckDisasm)
