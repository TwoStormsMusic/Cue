cmake_minimum_required(VERSION 3.20)
project(cue VERSION 0.3.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(CUE_BUILD_TESTS "Build the offline tests" ON)

find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

add_library(clap_headers INTERFACE)
target_include_directories(clap_headers INTERFACE third_party/clap/include)

# VST3 interfaces and the SDK's hosting helpers (MIT).
set(VST3_DIR third_party/vst3sdk)
add_library(vst3_hosting STATIC
  ${VST3_DIR}/pluginterfaces/base/coreiids.cpp
  ${VST3_DIR}/pluginterfaces/base/funknown.cpp
  ${VST3_DIR}/pluginterfaces/base/ustring.cpp
  ${VST3_DIR}/public.sdk/source/common/commonstringconvert.cpp
  ${VST3_DIR}/public.sdk/source/common/memorystream.cpp
  ${VST3_DIR}/public.sdk/source/common/threadchecker_linux.cpp
  ${VST3_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
  ${VST3_DIR}/public.sdk/source/vst/hosting/eventlist.cpp
  ${VST3_DIR}/public.sdk/source/vst/hosting/hostclasses.cpp
  ${VST3_DIR}/public.sdk/source/vst/hosting/module.cpp
  ${VST3_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
  ${VST3_DIR}/public.sdk/source/vst/hosting/parameterchanges.cpp
  ${VST3_DIR}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
  ${VST3_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
  ${VST3_DIR}/public.sdk/source/vst/hosting/processdata.cpp
  ${VST3_DIR}/public.sdk/source/vst/utility/stringconvert.cpp
  ${VST3_DIR}/public.sdk/source/vst/vstinitiids.cpp
)
target_include_directories(vst3_hosting PUBLIC ${VST3_DIR})
target_compile_definitions(vst3_hosting PUBLIC RELEASE=1)
target_compile_options(vst3_hosting PRIVATE -w)
set_target_properties(vst3_hosting PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(vst3_hosting PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)

add_library(cue_core STATIC
  src/core/engine.cpp
  src/core/chain.cpp
  src/core/processor.cpp
)
target_include_directories(cue_core PUBLIC src)

add_library(cue_host STATIC
  src/host/clap_processor.cpp
  src/host/vst3_processor.cpp
)
target_link_libraries(cue_host PUBLIC cue_core clap_headers vst3_hosting ${CMAKE_DL_LIBS})

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

add_executable(cue
  src/app/main.cpp
  src/app/build.cpp
  src/app/config.cpp
  src/app/rpp.cpp
  src/control/show.cpp
  src/app/cpu.cpp
  src/app/doctor.cpp
  src/audio/alsa_backend.cpp
  src/midi/alsa_midi.cpp
)
target_link_libraries(cue PRIVATE cue_host PkgConfig::ALSA Threads::Threads)
# Self-contained C++ runtime, so the binary runs on any distribution with glibc 2.38 or newer.
target_link_options(cue PRIVATE -static-libstdc++ -static-libgcc)

# Test plug-ins: one .clap file with gain, delay, burn and crash plug-ins.
add_library(cue_test_plugins MODULE plugins/test/test_plugins.cpp)
target_link_libraries(cue_test_plugins PRIVATE clap_headers)
set_target_properties(cue_test_plugins PROPERTIES PREFIX "" SUFFIX ".clap"
  OUTPUT_NAME "Cue Test Plugins" CXX_VISIBILITY_PRESET hidden)

# Test VST3: a bundle with the shared object at Contents/x86_64-linux.
set(TEST_VST3_BUNDLE "${CMAKE_BINARY_DIR}/Cue Test.vst3")
add_library(cue_test_vst3 MODULE plugins/test/test_vst3.cpp)
target_include_directories(cue_test_vst3 PRIVATE ${VST3_DIR})
target_compile_definitions(cue_test_vst3 PRIVATE RELEASE=1)
target_compile_options(cue_test_vst3 PRIVATE -Wno-unused-function)
set_target_properties(cue_test_vst3 PROPERTIES PREFIX "" SUFFIX ".so" OUTPUT_NAME "Cue Test"
  LIBRARY_OUTPUT_DIRECTORY "${TEST_VST3_BUNDLE}/Contents/x86_64-linux" CXX_VISIBILITY_PRESET hidden)
# The interfaces' IIDs are defined in funknown/coreiids sources the plug-in needs too.
target_sources(cue_test_vst3 PRIVATE
  ${VST3_DIR}/pluginterfaces/base/coreiids.cpp
  ${VST3_DIR}/pluginterfaces/base/funknown.cpp
  ${VST3_DIR}/public.sdk/source/vst/vstinitiids.cpp)

if(CUE_BUILD_TESTS)
  enable_testing()
  add_executable(cue_tests tests/main.cpp tests/test_engine.cpp tests/test_clap.cpp tests/test_vst3.cpp tests/test_config.cpp tests/test_show.cpp tests/test_rpp.cpp src/app/config.cpp src/app/rpp.cpp src/control/show.cpp)
  target_link_libraries(cue_tests PRIVATE cue_host PkgConfig::ALSA)
  target_compile_definitions(cue_tests PRIVATE
    CUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CUE_TEST_CLAP="$<TARGET_FILE:cue_test_plugins>"
    CUE_TEST_VST3="${TEST_VST3_BUNDLE}"
    CUE_TEST_VST3_SO="$<TARGET_FILE:cue_test_vst3>")
  add_dependencies(cue_tests cue_test_plugins cue_test_vst3)
  target_link_options(cue_tests PRIVATE -static-libstdc++ -static-libgcc)
  add_test(NAME cue_tests COMMAND cue_tests)
endif()
