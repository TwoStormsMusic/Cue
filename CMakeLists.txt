cmake_minimum_required(VERSION 3.20)
project(stagehost VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(STAGEHOST_BUILD_TESTS "Build the offline tests" ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_library(stagehost_core STATIC
  src/core/engine.cpp
  src/core/scene.cpp
)
target_include_directories(stagehost_core PUBLIC src)

if(STAGEHOST_BUILD_TESTS)
  enable_testing()
  add_executable(stagehost_tests tests/main.cpp tests/test_engine.cpp)
  target_link_libraries(stagehost_tests PRIVATE stagehost_core)
  add_test(NAME stagehost_tests COMMAND stagehost_tests)
endif()
