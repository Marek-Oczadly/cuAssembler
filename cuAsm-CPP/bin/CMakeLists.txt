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
