#!/usr/bin/env bash
# Builds a release and packs it as dist/stagehost-<version>-linux-x86_64.tar.gz
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="$(grep -oP 'project\(stagehost VERSION \K[0-9.]+' "$root/CMakeLists.txt")"
build="$root/build-release"
name="stagehost-$version-linux-x86_64"
stage="$root/dist/$name"

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build"

rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/plugins" "$stage/configs"
cp "$build/stagehost" "$build/stagehost_tests" "$stage/bin/"
cp "$build/Stagehost Test Plugins.clap" "$stage/plugins/"
cp -r "$build/Stagehost Test.vst3" "$stage/plugins/"
cp "$root"/configs/*.ini "$stage/configs/"
cp "$root/scripts/run-tests.sh" "$root/scripts/rig-info.sh" "$stage/"
cp "$root/README.md" "$root/TESTING.md" "$stage/"
strip "$stage/bin/stagehost" "$stage/bin/stagehost_tests"
tar -C "$root/dist" -czf "$root/dist/$name.tar.gz" "$name"
echo "$root/dist/$name.tar.gz"
