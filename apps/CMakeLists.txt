cmake_minimum_required (VERSION 3.5.1 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 11)

add_subdirectory (block-perf)
add_subdirectory (paging-tracker)

find_package(CUDA)
if(CUDA_FOUND AND CUDA_VERSION GREATER 7.5)
  add_subdirectory(cuda-dma)
  add_subdirectory(gpu-direct)
endif()
