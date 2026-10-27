cmake_minimum_required(VERSION 3.20)
project(cue VERSION 0.9.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(CUE_BUILD_TESTS "Build the offline tests" ON)
option(CUE_BUILD_APP "Build the desktop app, cue-app (needs Qt 6 Widgets and Network)" ON)

find_package(Threads REQUIRED)
# X11, for the windows plug-in editors live in. Without it the engine still
# builds and says editors are not available.
find_package(X11)

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
add_compile_definitions(CUE_VERSION="${PROJECT_VERSION}")

# .cue files: zip (miniz, MIT) and JSON (nlohmann/json, MIT).
add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
target_compile_options(miniz PRIVATE -w)
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE third_party)

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
  src/tuner/fft.cpp
  src/tuner/pitch.cpp
  src/tuner/tuner.cpp
)
target_include_directories(cue_core PUBLIC src)

add_library(cue_host STATIC
  src/host/clap_processor.cpp
  src/host/editor_host.cpp
  src/host/vst3_processor.cpp
)
target_link_libraries(cue_host PUBLIC cue_core clap_headers vst3_hosting ${CMAKE_DL_LIBS})
if(X11_FOUND)
  target_compile_definitions(cue_host PRIVATE CUE_HAVE_X11)
  target_link_libraries(cue_host PUBLIC X11::X11)
else()
  message(STATUS "X11 not found: plug-in editors will not open (install libx11-dev)")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

add_executable(cue
  src/app/main.cpp
  src/app/build.cpp
  src/app/edit.cpp
  src/app/arrange.cpp
  src/app/config.cpp
  src/app/cuefile.cpp
  src/app/service.cpp
  src/app/rpp.cpp
  src/app/scan.cpp
  src/control/show.cpp
  src/app/cpu.cpp
  src/app/doctor.cpp
  src/audio/alsa_backend.cpp
  src/audio/take_device.cpp
  src/midi/alsa_midi.cpp
)
target_link_libraries(cue PRIVATE cue_host miniz nlohmann_json PkgConfig::ALSA Threads::Threads)
# Self-contained C++ runtime, so the binary runs on any distribution with glibc 2.38 or newer.
target_link_options(cue PRIVATE -static-libstdc++ -static-libgcc)

# Test plug-ins: one .clap file with gain, delay, burn and crash plug-ins.
add_library(cue_test_plugins MODULE plugins/test/test_plugins.cpp)
target_link_libraries(cue_test_plugins PRIVATE clap_headers)
if(X11_FOUND)
  # cue.test.gain gets a small editor, so the host's editor windows can be
  # tested without an outside plug-in.
  target_compile_definitions(cue_test_plugins PRIVATE CUE_TEST_X11)
  target_link_libraries(cue_test_plugins PRIVATE X11::X11)
endif()
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

# The desktop app: Qt 6 Widgets, drawn by hand; talks to cue run over its socket.
if(CUE_BUILD_APP)
  find_package(Qt6 6.2 COMPONENTS Widgets Network QUIET)
  if(Qt6_FOUND)
    add_library(cue_gui STATIC
      src/gui/link.cpp
      src/gui/model.cpp
      src/gui/state.cpp
      src/gui/theme.cpp
      src/gui/surface.cpp
      src/gui/stageview.cpp
      src/gui/tunerview.cpp
      src/gui/board.cpp
      src/gui/chainview.cpp
      src/gui/chainsettings.cpp
      src/gui/pluginsview.cpp
      src/gui/settingsview.cpp
      src/gui/mainwindow.cpp
      src/gui/fonts.qrc
    )
    set_target_properties(cue_gui PROPERTIES AUTOMOC ON AUTORCC ON)
    target_include_directories(cue_gui PUBLIC src)
    target_link_libraries(cue_gui PUBLIC Qt6::Widgets Qt6::Network)
    add_executable(cue-app src/gui/main.cpp)
    target_link_libraries(cue-app PRIVATE cue_gui)
  else()
    message(STATUS "Qt 6 not found: cue-app is not built (install qt6-base-dev)")
    set(CUE_BUILD_APP OFF)
  endif()
endif()

if(CUE_BUILD_TESTS)
  enable_testing()
  add_executable(cue_tests tests/main.cpp tests/test_engine.cpp tests/test_graph.cpp tests/test_clap.cpp tests/test_editor.cpp tests/test_vst3.cpp tests/test_config.cpp tests/test_show.cpp tests/test_rpp.cpp tests/test_cuefile.cpp tests/test_service.cpp tests/test_tuner.cpp tests/test_scan.cpp tests/test_arrange.cpp tests/test_device.cpp src/audio/take_device.cpp src/app/config.cpp src/app/cuefile.cpp src/app/service.cpp src/app/rpp.cpp src/app/scan.cpp src/app/build.cpp src/app/edit.cpp src/app/arrange.cpp src/control/show.cpp)
  target_link_libraries(cue_tests PRIVATE cue_host miniz nlohmann_json PkgConfig::ALSA)
  target_compile_definitions(cue_tests PRIVATE
    CUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CUE_TEST_CLAP="$<TARGET_FILE:cue_test_plugins>"
    CUE_TEST_VST3="${TEST_VST3_BUNDLE}"
    CUE_TEST_VST3_SO="$<TARGET_FILE:cue_test_vst3>"
    CUE_BIN="$<TARGET_FILE:cue>")
  if(X11_FOUND)
    # The editor tests look at the windows the way a window manager would.
    target_compile_definitions(cue_tests PRIVATE CUE_HAVE_X11)
  endif()
  add_dependencies(cue_tests cue_test_plugins cue_test_vst3 cue)
  target_link_options(cue_tests PRIVATE -static-libstdc++ -static-libgcc)
  add_test(NAME cue_tests COMMAND cue_tests)

  if(CUE_BUILD_APP)
    find_package(Qt6 COMPONENTS Test REQUIRED)
    add_executable(cue_app_tests tests/app/test_app.cpp)
    set_target_properties(cue_app_tests PROPERTIES AUTOMOC ON)
    target_link_libraries(cue_app_tests PRIVATE cue_gui Qt6::Test)
    target_compile_definitions(cue_app_tests PRIVATE
      CUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
      CUE_BIN="$<TARGET_FILE:cue>"
      CUE_TEST_CLAP="$<TARGET_FILE:cue_test_plugins>")
    add_dependencies(cue_app_tests cue cue_test_plugins)
    add_test(NAME cue_app_tests COMMAND cue_app_tests)
    set_tests_properties(cue_app_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
  endif()
endif()
