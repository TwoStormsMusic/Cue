# CMake generated Testfile for 
# Source directory: /home/claude/cue
# Build directory: /home/claude/cue/build-merge
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[cue_tests]=] "/home/claude/cue/build-merge/cue_tests")
set_tests_properties([=[cue_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/claude/cue/CMakeLists.txt;181;add_test;/home/claude/cue/CMakeLists.txt;0;")
add_test([=[cue_app_tests]=] "/home/claude/cue/build-merge/cue_app_tests")
set_tests_properties([=[cue_app_tests]=] PROPERTIES  ENVIRONMENT "QT_QPA_PLATFORM=offscreen" _BACKTRACE_TRIPLES "/home/claude/cue/CMakeLists.txt;193;add_test;/home/claude/cue/CMakeLists.txt;0;")
