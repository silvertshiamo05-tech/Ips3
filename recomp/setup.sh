#!/usr/bin/env bash
# Builds the recompilation toolchain into recomp/build: XenonRecomp/XenonAnalyse at a pinned
# commit, plus xex_dump. Linux or macOS, needs clang >= 18, cmake, ninja, python3.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"
XR_COMMIT=ddd128bcca99fe8bfbb99bea583c972351fa6ace

mkdir -p "$BUILD"
if [ ! -d "$BUILD/XenonRecomp/.git" ]; then
	git clone --recursive https://github.com/hedge-dev/XenonRecomp.git "$BUILD/XenonRecomp"
fi
git -C "$BUILD/XenonRecomp" checkout -q "$XR_COMMIT"
git -C "$BUILD/XenonRecomp" submodule update --init --recursive

XR="$BUILD/XenonRecomp"
cmake -S "$XR" -B "$XR/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C "$XR/build" XenonAnalyse XenonRecomp

clang++ -std=c++20 -O2 "$HERE/tools/xex_dump.cpp" \
	-I"$XR/XenonUtils" -I"$XR/thirdparty" -I"$XR/thirdparty/fmt/include" -I"$XR/thirdparty/disasm" \
	"$XR/build/XenonUtils/libXenonUtils.a" $(find "$XR/build/thirdparty" -name '*.a') \
	-o "$BUILD/xex_dump"

echo "toolchain ready in $BUILD"
