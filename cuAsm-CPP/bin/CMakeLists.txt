# Target is named cuasm_cli (not cuasm) to avoid clashing with the CuAsm library target: VS
# solution/project names are case-insensitive, so "cuasm" and "CuAsm" collide there even though
# CMake itself treats them as distinct targets.
add_executable(cuasm_cli cuasm.cpp)
target_link_libraries(cuasm_cli PRIVATE CuAsm CLI11::CLI11)
set_target_properties(cuasm_cli PROPERTIES OUTPUT_NAME cuasm)

add_executable(dsass dsass.cpp)
target_link_libraries(dsass PRIVATE CuAsm CLI11::CLI11)

add_executable(hcubin hcubin.cpp)
target_link_libraries(hcubin PRIVATE CuAsm CLI11::CLI11)

add_executable(hnvcc hnvcc.cpp)
target_link_libraries(hnvcc PRIVATE CuAsm)

# verify-cc/correct-cc: CLI skeletons for control-code verification/correction. Their actual
# hazard analysis (ccCommon.hpp's verifyControlCodes/correctControlCodes) is not implemented yet
# -- see Reports/control-codes-validation.md.
add_executable(verify-cc verify-cc.cpp)
target_link_libraries(verify-cc PRIVATE CuAsm CLI11::CLI11 elfio::elfio)

add_executable(correct-cc correct-cc.cpp)
target_link_libraries(correct-cc PRIVATE CuAsm CLI11::CLI11 elfio::elfio)
