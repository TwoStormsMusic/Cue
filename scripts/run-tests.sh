#!/usr/bin/env bash
# Runs the offline tests and two short benches from an unpacked package.
# Needs no audio hardware and changes nothing on the system.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
export STAGEHOST_TEST_CLAP="$here/plugins/Stagehost Test Plugins.clap"
export STAGEHOST_TEST_VST3="$here/plugins/Stagehost Test.vst3"
export STAGEHOST_TEST_VST3_SO="$here/plugins/Stagehost Test.vst3/Contents/x86_64-linux/Stagehost Test.so"

status=0
echo "== unit tests"
"$here/bin/stagehost_tests" || status=1

echo
echo "== bench: built-in blocks, 15 s"
"$here/bin/stagehost" bench "$here/configs/builtin-demo.ini" --seconds 15 || status=1

echo
echo "== bench: test plug-ins (CLAP and VST3), 15 s"
"$here/bin/stagehost" bench "$here/configs/test-plugins.ini" --seconds 15 || status=1

echo
[ $status -eq 0 ] && echo "all done" || echo "something failed: send the output above"
exit $status
