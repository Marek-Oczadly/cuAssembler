add_executable(cuasm cuasm.cpp)
target_link_libraries(cuasm PRIVATE CuAsm)

add_executable(dsass dsass.cpp)
target_link_libraries(dsass PRIVATE CuAsm)

add_executable(hcubin hcubin.cpp)
target_link_libraries(hcubin PRIVATE CuAsm)

add_executable(hnvcc hnvcc.cpp)
target_link_libraries(hnvcc PRIVATE CuAsm)
