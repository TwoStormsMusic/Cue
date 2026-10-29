# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Release")
  file(REMOVE_RECURSE
  "CMakeFiles/cue-convert_autogen.dir/AutogenUsed.txt"
  "CMakeFiles/cue-convert_autogen.dir/ParseCache.txt"
  "CMakeFiles/cue_app_tests_autogen.dir/AutogenUsed.txt"
  "CMakeFiles/cue_app_tests_autogen.dir/ParseCache.txt"
  "CMakeFiles/cue_gui_autogen.dir/AutogenUsed.txt"
  "CMakeFiles/cue_gui_autogen.dir/ParseCache.txt"
  "cue-convert_autogen"
  "cue_app_tests_autogen"
  "cue_gui_autogen"
  )
endif()
