#!/usr/bin/env bash
# Builds LLVM (AArch64 only, static) for iOS at the exact commit ARMSX3 pins, and installs
# it to $LLVM_IOS_DIR for the core to link against (-DBUILD_LLVM=OFF -DLLVM_DIR=...).
#
# Cross-compiling LLVM needs host tablegen binaries, so a small host build comes first.
set -euo pipefail
source "$(dirname "$0")/common.sh"

SHA="$(llvm_commit)"
SRC="$BUILD/llvm-src"
HOST="$BUILD/llvm-host"
TARGET="$BUILD/llvm-ios-build"

log "LLVM $SHA -> $LLVM_IOS_DIR (iOS $IOS_DEPLOYMENT_TARGET)"

if [ ! -d "$SRC/.git" ]; then
	git init -q "$SRC"
	git -C "$SRC" remote add origin https://github.com/llvm/llvm-project.git
fi

if [ "$(git -C "$SRC" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$SHA" ]; then
	for attempt in 1 2 3 4; do
		git -C "$SRC" fetch --depth 1 origin "$SHA" && break
		sleep $((attempt * 4))
	done
	git -C "$SRC" checkout -q FETCH_HEAD
fi

common_llvm_flags=(
	-DCMAKE_BUILD_TYPE=Release
	-DLLVM_TARGETS_TO_BUILD=AArch64
	-DLLVM_INCLUDE_TESTS=OFF
	-DLLVM_INCLUDE_BENCHMARKS=OFF
	-DLLVM_INCLUDE_EXAMPLES=OFF
	-DLLVM_INCLUDE_DOCS=OFF
	-DLLVM_ENABLE_BINDINGS=OFF
	-DLLVM_ENABLE_ZLIB=OFF
	-DLLVM_ENABLE_ZSTD=OFF
	-DLLVM_ENABLE_LIBXML2=OFF
	-DLLVM_ENABLE_LIBEDIT=OFF
	-DLLVM_ENABLE_LIBPFM=OFF
	-DLLVM_ENABLE_TERMINFO=OFF
	-DLLVM_ENABLE_ASSERTIONS=OFF
	-DLLVM_ENABLE_WARNINGS=OFF
)

log "Host tablegen"
cmake -S "$SRC/llvm" -B "$HOST" -G Ninja "${common_llvm_flags[@]}"
host_tools=(llvm-tblgen)
if ninja -C "$HOST" -t targets all | grep -q '^llvm-min-tblgen:'; then
	host_tools+=(llvm-min-tblgen)
fi
ninja -C "$HOST" -j"$JOBS" "${host_tools[@]}"

log "LLVM for iOS"
cmake -S "$SRC/llvm" -B "$TARGET" -G Ninja "${common_llvm_flags[@]}" \
	-DCMAKE_SYSTEM_NAME=iOS \
	-DCMAKE_OSX_SYSROOT=iphoneos \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOYMENT_TARGET" \
	-DCMAKE_INSTALL_PREFIX="$LLVM_IOS_DIR" \
	-DLLVM_TARGET_ARCH=AArch64 \
	-DLLVM_HOST_TRIPLE="arm64-apple-ios$IOS_DEPLOYMENT_TARGET" \
	-DLLVM_DEFAULT_TARGET_TRIPLE="arm64-apple-ios$IOS_DEPLOYMENT_TARGET" \
	-DLLVM_NATIVE_TOOL_DIR="$HOST/bin" \
	-DLLVM_TABLEGEN="$HOST/bin/llvm-tblgen" \
	-DLLVM_BUILD_TOOLS=OFF \
	-DLLVM_INCLUDE_TOOLS=OFF \
	-DLLVM_BUILD_UTILS=OFF \
	-DLLVM_INCLUDE_UTILS=OFF \
	-DLLVM_BUILD_RUNTIME=OFF \
	-DLLVM_ENABLE_PLUGINS=OFF \
	-DLLVM_ENABLE_PIC=ON \
	-DBUILD_SHARED_LIBS=OFF

ninja -C "$TARGET" -j"$JOBS" install

log "Installed LLVM to $LLVM_IOS_DIR"
ls "$LLVM_IOS_DIR/lib" | head -50
