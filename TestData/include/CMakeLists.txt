# Header-only helpers (cuptr.hpp, hostptr.hpp, helper_cuda.h, helper_string.h, util.h) shared by
# the CUDA microbenchmarks under TestData/microbench/*.

add_library(TestDataInclude INTERFACE)
target_include_directories(TestDataInclude INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
