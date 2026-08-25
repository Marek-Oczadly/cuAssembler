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

add_executable(test_CuSMVersion test_CuSMVersion.cpp)
target_link_libraries(test_CuSMVersion PRIVATE CuAsm)
add_test(NAME test_CuSMVersion COMMAND test_CuSMVersion)

add_executable(test_UnsupportedArch test_UnsupportedArch.cpp)
target_link_libraries(test_UnsupportedArch PRIVATE CuAsm)
add_test(NAME test_UnsupportedArch COMMAND test_UnsupportedArch)

add_executable(test_CuControlCode test_CuControlCode.cpp)
target_link_libraries(test_CuControlCode PRIVATE CuAsm)
add_test(NAME test_CuControlCode COMMAND test_CuControlCode)

add_executable(test_CuNVInfo test_CuNVInfo.cpp)
target_link_libraries(test_CuNVInfo PRIVATE CuAsm elfio::elfio)
add_test(NAME test_CuNVInfo COMMAND test_CuNVInfo)

add_executable(test_common test_common.cpp)
target_link_libraries(test_common PRIVATE CuAsm)
add_test(NAME test_common COMMAND test_common)

add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE CuAsm)
add_test(NAME test_config COMMAND test_config)

add_executable(test_CuInsFeeder test_CuInsFeeder.cpp)
target_link_libraries(test_CuInsFeeder PRIVATE CuAsm)
add_test(NAME test_CuInsFeeder COMMAND test_CuInsFeeder)

add_executable(test_OperandRole test_OperandRole.cpp)
target_link_libraries(test_OperandRole PRIVATE CuAsm)
add_test(NAME test_OperandRole COMMAND test_OperandRole)

add_subdirectory(utils)
add_subdirectory(CheckDisasm)
