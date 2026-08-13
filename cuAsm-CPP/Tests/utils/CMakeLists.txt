# Unit tests for CuAsm/utils/ - previously entirely untested (no test anywhere referenced
# BigNum, CubinUtils, FileTemplate, OrderedMap, or RationalMatrix by name). Each of these operates
# purely on local data/files, with no nvcc/nvdisasm/cuobjdump subprocess dependency, so unlike
# Tests/CheckDisasm they run without a CUDA toolkit installed.

add_executable(test_BigNum test_BigNum.cpp)
target_link_libraries(test_BigNum PRIVATE CuAsm)
add_test(NAME test_BigNum COMMAND test_BigNum)

add_executable(test_OrderedMap test_OrderedMap.cpp)
target_link_libraries(test_OrderedMap PRIVATE CuAsm)
add_test(NAME test_OrderedMap COMMAND test_OrderedMap)

add_executable(test_RationalMatrix test_RationalMatrix.cpp)
target_link_libraries(test_RationalMatrix PRIVATE CuAsm)
add_test(NAME test_RationalMatrix COMMAND test_RationalMatrix)

add_executable(test_FileTemplate test_FileTemplate.cpp)
target_link_libraries(test_FileTemplate PRIVATE CuAsm)
add_test(NAME test_FileTemplate COMMAND test_FileTemplate)

add_executable(test_CubinUtils test_CubinUtils.cpp)
target_link_libraries(test_CubinUtils PRIVATE CuAsm)
add_test(NAME test_CubinUtils COMMAND test_CubinUtils)
