#!/usr/bin/env bash
# Builds a release and packs it as dist/cue-<version>-linux-x86_64.tar.gz
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="$(grep -oP 'project\(cue VERSION \K[0-9.]+' "$root/CMakeLists.txt")"
build="$root/build-release"
name="cue-$version-linux-x86_64"
stage="$root/dist/$name"

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build"

rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/plugins" "$stage/configs" "$stage/docs"
cp "$build/cue" "$build/cue_tests" "$stage/bin/"
# The app, when Qt 6 was there to build it.
for f in cue-app cue_app_tests; do
  [ -x "$build/$f" ] && cp "$build/$f" "$stage/bin/"
done
cp "$build/Cue Test Plugins.clap" "$stage/plugins/"
cp -r "$build/Cue Test.vst3" "$stage/plugins/"
cp "$root"/configs/*.ini "$stage/configs/"
cp "$root/scripts/run-tests.sh" "$root/scripts/rig-info.sh" "$stage/"
cp "$root/README.md" "$root/TESTING.md" "$stage/"
cp "$root"/docs/*.md "$stage/docs/"
if [ -f "$build/cue-convert" ]; then
  cp "$build/cue-convert" "$stage/bin/"
fi
strip "$stage"/bin/*
mkdir -p "$stage/licenses"
cp "$root"/third_party/fonts/OFL-*.txt "$stage/licenses/"
tar -C "$root/dist" -czf "$root/dist/$name.tar.gz" "$name"
echo "$root/dist/$name.tar.gz"
