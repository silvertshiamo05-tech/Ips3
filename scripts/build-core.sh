#!/usr/bin/env bash
# Configures and builds Ips3Core.framework (ARMSX3/RPCS3 core + iOS bridge) for iOS arm64.
# Needs build-llvm-ios.sh, build-ffmpeg-ios.sh and fetch-moltenvk.sh to have run.
set -euo pipefail
source "$(dirname "$0")/common.sh"

OUT="$BUILD/core"

"$ROOT/scripts/apply-patches.sh"

# Upstream adds these only for the "Clang" compiler id, which Xcode's AppleClang is not.
cxx_flags=(
	-fconstexpr-steps=16777216
	-Wno-unused-lambda-capture
	-Wno-unused-private-field
	-Wno-elaborated-enum-base
	-Wno-unused-command-line-argument
	-Wno-deprecated-declarations
)

log "Configure core"
cmake -S "$CORE" -B "$OUT" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_SYSTEM_NAME=iOS \
	-DCMAKE_OSX_SYSROOT=iphoneos \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOYMENT_TARGET" \
	`# CMake leaves this empty when cross-compiling; RPCS3 keys its AArch64 backend on it` \
	-DCMAKE_SYSTEM_PROCESSOR=arm64 \
	-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
	-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
	-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
	`# never pick up the build host's Homebrew libraries (they are macOS binaries)` \
	-DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
	-DCCACHE_PATH="$(command -v ccache || true)" \
	-DCMAKE_CXX_FLAGS="${cxx_flags[*]}" \
	-DCMAKE_OBJCXX_FLAGS="${cxx_flags[*]}" \
	-DIPS3_IOS_DIR="$ROOT/ios/core" \
	-DFFMPEG_IOS_DIR="$FFMPEG_IOS_DIR" \
	-DUSE_NATIVE_INSTRUCTIONS=OFF \
	-DUSE_PRECOMPILED_HEADERS=OFF \
	-DUSE_LTO=OFF \
	-DBUILD_RPCS3_TESTS=OFF \
	-DUSE_SDL=OFF \
	-DUSE_SYSTEM_SDL=OFF \
	-DUSE_FAUDIO=OFF \
	-DUSE_LIBEVDEV=OFF \
	-DUSE_DISCORD_RPC=OFF \
	-DUSE_GAMEMODE=OFF \
	-DUSE_SYSTEM_LIBUSB=OFF \
	-DUSE_SYSTEM_CURL=OFF \
	-DUSE_SYSTEM_OPENCV=OFF \
	-DUSE_SYSTEM_FFMPEG=OFF \
	-DUSE_SYSTEM_ZLIB=ON \
	-DUSE_SYSTEM_MVK=ON \
	-DVulkan_INCLUDE_DIR="$MOLTENVK_DIR/include" \
	-DVulkan_LIBRARY="$MOLTENVK_DIR/lib/libMoltenVK.a" \
	-DWITH_LLVM=ON \
	-DBUILD_LLVM=OFF \
	-DSTATIC_LINK_LLVM=ON \
	-DLLVM_DIR="$LLVM_IOS_DIR/lib/cmake/llvm" \
	-DLLVM_TARGETS_TO_BUILD=AArch64 \
	-DASMJIT_NO_SHM_OPEN=ON \
	`# cubeb's AudioUnit backend is macOS-only; iOS audio is CoreAudioBackend in ios/core` \
	-DUSE_AUDIOUNIT=0

log "Build core"
ninja -C "$OUT" -j"$JOBS" -k 0 Ips3Core

fw="$(find "$OUT" -name Ips3Core.framework -type d -maxdepth 4 | head -n1)"
log "Built $fw"
ls -la "$fw"
