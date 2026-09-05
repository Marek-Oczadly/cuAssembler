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

add_executable(test_ParsedCubin test_ParsedCubin.cpp)
target_link_libraries(test_ParsedCubin PRIVATE CuAsm elfio::elfio Boost::graph)
add_test(NAME test_ParsedCubin COMMAND test_ParsedCubin)

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

add_executable(test_LatencyClass test_LatencyClass.cpp)
target_link_libraries(test_LatencyClass PRIVATE CuAsm)
add_test(NAME test_LatencyClass COMMAND test_LatencyClass)

# Exercises bin/ccCommon.hpp's Phase 2 decode layer (DecodedInstruction / decodeInstructionsFromSass /
# decodeInstructions), not just CuAsm/ library code -- links elfio::elfio directly since ccCommon.hpp
# uses ELFIO::elfio itself (CuAsm already links it PUBLIC, but this makes the dependency explicit,
# matching test_elfSection/test_nvinfo's precedent). Boost::graph is needed too since ccCommon.hpp's
# hazard graph (Phase 3) is a boost::adjacency_list, pulled in by the same header.
add_executable(test_ccCommon test_ccCommon.cpp)
target_link_libraries(test_ccCommon PRIVATE CuAsm elfio::elfio Boost::graph)
add_test(NAME test_ccCommon COMMAND test_ccCommon)

# Exercises bin/ccCommon.hpp's Phase 3 hazard graph builder/scoreboard simulator (buildHazardGraph/
# simulateAndVerify/verifyControlCodes) against hand-built synthetic DecodedInstruction sequences,
# per Reports/tasks.md Phase 5's "prefer synthetic instruction sequences for hazard/scoreboard-
# simulator unit tests" guidance -- no cuobjdump/real cubin needed here.
add_executable(test_ccHazard test_ccHazard.cpp)
target_link_libraries(test_ccHazard PRIVATE CuAsm elfio::elfio Boost::graph)
add_test(NAME test_ccHazard COMMAND test_ccHazard)

# Exercises bin/ccCommon.hpp's Phase 4 correction layer (collectBarrierIntervals/assignBarrierSlots/
# correctControlCodes) against hand-built synthetic DecodedInstruction sequences, same rationale as
# test_ccHazard.cpp -- no cuobjdump/real cubin needed here.
add_executable(test_ccCorrect test_ccCorrect.cpp)
target_link_libraries(test_ccCorrect PRIVATE CuAsm elfio::elfio Boost::graph)
add_test(NAME test_ccCorrect COMMAND test_ccCorrect)

# Exercises bin/ccCommon.hpp's computeControlFlowSuccessors() directly against hand-built
# synthetic DecodedInstruction sequences, in particular its handling of an ".ABS"-modified BRA/JMP
# immediate target (insVals.back() is a raw absolute address for those, not a PC-relative offset --
# see CuInsParser::specialTreatment()'s own "ABS" check) -- no cuobjdump/real cubin needed here.
add_executable(test_ccControlFlow test_ccControlFlow.cpp)
target_link_libraries(test_ccControlFlow PRIVATE CuAsm elfio::elfio Boost::graph)
add_test(NAME test_ccControlFlow COMMAND test_ccControlFlow)

add_subdirectory(utils)
add_subdirectory(CheckDisasm)
add_subdirectory(CheckControlCodes)
add_subdirectory(CheckControlCodesShuffle)
add_subdirectory(CheckControlCodesRoundtrip)
add_subdirectory(CheckControlCodesLaunch)
