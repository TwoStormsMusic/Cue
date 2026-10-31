#!/usr/bin/env bash
# Runs the offline tests and two short benches from an unpacked package.
# Needs no audio hardware and changes nothing on the system.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
export CUE_TEST_CLAP="$here/plugins/Cue Test Plugins.clap"
export CUE_TEST_VST3="$here/plugins/Cue Test.vst3"
export CUE_TEST_VST3_SO="$here/plugins/Cue Test.vst3/Contents/x86_64-linux/Cue Test.so"
export CUE_CONFIGS="$here/configs"
export CUE_BIN="$here/bin/cue"

status=0
echo "== unit tests"
"$here/bin/cue_tests" || status=1

if [ -x "$here/bin/cue_app_tests" ]; then
    echo
    echo "== app tests (drawn offscreen, against bin/cue run --audio none)"
    CUE_BIN="$here/bin/cue" QT_QPA_PLATFORM=offscreen "$here/bin/cue_app_tests" || status=1
fi

echo
echo "== bench: built-in blocks, 15 s"
"$here/bin/cue" bench "$here/configs/builtin-demo.ini" --seconds 15 || status=1

echo
echo "== bench: test plug-ins (CLAP and VST3), 15 s"
"$here/bin/cue" bench "$here/configs/test-plugins.ini" --seconds 15 || status=1

echo
[ $status -eq 0 ] && echo "all done" || echo "something failed: send the output above"
exit $status
