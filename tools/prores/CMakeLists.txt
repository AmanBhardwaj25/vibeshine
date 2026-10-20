cmake_minimum_required(VERSION 3.20)
project(prores_tools CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(PRORES_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/prores)
add_library(prores_ref STATIC ${PRORES_SRC}/prores_bitstream.cpp)
target_include_directories(prores_ref PUBLIC ${PRORES_SRC})

add_executable(prores_selftest prores_selftest.cpp)
target_link_libraries(prores_selftest prores_ref)

add_executable(prores_ref_encode prores_ref_encode.cpp)
target_link_libraries(prores_ref_encode prores_ref)

if(WIN32)
  set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
  add_executable(prores_gpu_test prores_gpu_test.cpp ${REPO_ROOT}/src/platform/windows/prores_d3d11_core.cpp)
  target_include_directories(prores_gpu_test PRIVATE ${REPO_ROOT})
  target_link_libraries(prores_gpu_test prores_ref d3d11 dxgi d3dcompiler)
endif()
