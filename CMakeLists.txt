cmake_minimum_required(VERSION 3.20)
project(stagehost VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(STAGEHOST_BUILD_TESTS "Build the offline tests" ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

add_library(clap_headers INTERFACE)
target_include_directories(clap_headers INTERFACE third_party/clap/include)

add_library(stagehost_core STATIC
  src/core/engine.cpp
  src/core/scene.cpp
)
target_include_directories(stagehost_core PUBLIC src)

add_library(stagehost_host STATIC
  src/host/clap_processor.cpp
)
target_link_libraries(stagehost_host PUBLIC stagehost_core clap_headers ${CMAKE_DL_LIBS})

# Test plug-ins: one .clap file with gain, delay, burn and crash plug-ins.
add_library(stagehost_test_plugins MODULE plugins/test/test_plugins.cpp)
target_link_libraries(stagehost_test_plugins PRIVATE clap_headers)
set_target_properties(stagehost_test_plugins PROPERTIES PREFIX "" SUFFIX ".clap"
  OUTPUT_NAME "Stagehost Test Plugins" CXX_VISIBILITY_PRESET hidden)

if(STAGEHOST_BUILD_TESTS)
  enable_testing()
  add_executable(stagehost_tests tests/main.cpp tests/test_engine.cpp tests/test_clap.cpp)
  target_link_libraries(stagehost_tests PRIVATE stagehost_host)
  target_compile_definitions(stagehost_tests PRIVATE
    STAGEHOST_TEST_CLAP="$<TARGET_FILE:stagehost_test_plugins>")
  add_dependencies(stagehost_tests stagehost_test_plugins)
  add_test(NAME stagehost_tests COMMAND stagehost_tests)
endif()
