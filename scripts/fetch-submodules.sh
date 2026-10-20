#!/usr/bin/env bash
# Shallow-fetches the ARMSX3 submodules the iOS build compiles.
#
# Skipped on purpose: LLVM (built separately by build-llvm-ios.sh, it is ~2 GB of history),
# the prebuilt desktop FFmpeg (built for iOS by build-ffmpeg-ios.sh), OpenCV, SDL, FAudio,
# Oboe and discord-rpc, none of which the iOS build uses.
set -euo pipefail
source "$(dirname "$0")/common.sh"

git -C "$ROOT" submodule update --init --depth 1 core/armsx3

needed=(
	3rdparty/asmjit/asmjit
	3rdparty/glslang/glslang
	3rdparty/zlib/zlib
	3rdparty/hidapi/hidapi
	3rdparty/pugixml
	3rdparty/yaml-cpp/yaml-cpp
	3rdparty/libpng/libpng
	3rdparty/libusb/libusb
	3rdparty/curl/curl
	3rdparty/wolfssl/wolfssl
	3rdparty/cubeb/cubeb
	3rdparty/SoundTouch/soundtouch
	3rdparty/miniupnp/miniupnp
	3rdparty/rtmidi/rtmidi
	3rdparty/zstd/zstd
	3rdparty/7zip/7zip
	3rdparty/OpenAL/openal-soft
	3rdparty/stblib/stb
	3rdparty/fusion/fusion
	3rdparty/GPUOpen/VulkanMemoryAllocator
	3rdparty/feralinteractive/feralinteractive
	3rdparty/protobuf/protobuf
	3rdparty/libchdr
	3rdparty/discord-rpc/discord-rpc
)

for attempt in 1 2 3 4; do
	if git -C "$CORE" submodule update --init --depth 1 --jobs 8 -- "${needed[@]}"; then
		exit 0
	fi
	sleep $((attempt * 4))
done

exit 1
