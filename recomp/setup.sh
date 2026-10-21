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

# Our changes: the instructions Bayonetta needs, CTR branches and switch cases that leave a
# function, and function analysis that respects known function starts.
for patch in "$HERE"/patches/*.patch; do
	[ -e "$patch" ] || continue
	if git -C "$BUILD/XenonRecomp" apply --reverse --check "$patch" 2>/dev/null; then
		echo "already applied: $(basename "$patch")"
	else
		git -C "$BUILD/XenonRecomp" apply "$patch"
	fi
done

XR="$BUILD/XenonRecomp"
# IPS3_SOURCES_ONLY=1: fetch and patch everything, but skip building the recompiler (enough to
# build the runtime, e.g. in CI against a stand-in for the game code).
if [ -z "${IPS3_SOURCES_ONLY:-}" ]; then
	cmake -S "$XR" -B "$XR/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
	ninja -C "$XR/build" XenonAnalyse XenonRecomp

	clang++ -std=c++20 -O2 "$HERE/tools/xex_dump.cpp" \
		-I"$XR/XenonUtils" -I"$XR/thirdparty" -I"$XR/thirdparty/fmt/include" -I"$XR/thirdparty/disasm" \
		"$XR/build/XenonUtils/libXenonUtils.a" $(find "$XR/build/thirdparty" -name '*.a') \
		-o "$BUILD/xex_dump"
fi

# The runtime's GPU: Xenia's Xenos emulation and Vulkan backend (BSD-3-Clause), at a pinned
# commit, with only the source directories it needs, and the libraries it builds on at the
# revisions Xenia pins. patches/xenia/*.patch adapts it to the Ips3 runtime.
XENIA_COMMIT=e6cd2e4bc998609ca25e24a24c8e32b66c5908ad
XENIA="$BUILD/xenia"
if [ ! -d "$XENIA/.git" ]; then
	git clone --filter=blob:none --sparse --no-checkout https://github.com/xenia-canary/xenia-canary.git "$XENIA"
fi
git -C "$XENIA" sparse-checkout set src/xenia/gpu src/xenia/base src/xenia/ui tools/build \
	third_party/renderdoc third_party/microprofile third_party/stb
git -C "$XENIA" checkout -q "$XENIA_COMMIT"

fetch_pinned() { # name url commit
	local dir="$XENIA/third_party/$1"
	if [ ! -d "$dir/.git" ]; then
		git init -q "$dir"
		git -C "$dir" remote add origin "$2"
	fi
	if [ "$(git -C "$dir" rev-parse -q --verify HEAD 2>/dev/null)" != "$3" ]; then
		git -C "$dir" fetch -q --depth 1 origin "$3"
		git -C "$dir" checkout -q FETCH_HEAD
	fi
}
fetch_pinned glslang https://github.com/KhronosGroup/glslang.git a57276bf558f5cf94d3a9854ebdf5a2236849a5a
fetch_pinned Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git 2cd90f9d20df57eac214c148f3aed885372ddcfe
fetch_pinned VulkanMemoryAllocator https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git 1d8f600fd424278486eade7ed3e877c99f0846b1
fetch_pinned cxxopts https://github.com/jarro2783/cxxopts.git f029892dab60526184ee42ae63a7f6d19a6b0e49
fetch_pinned tomlplusplus https://github.com/marzer/tomlplusplus.git 708fff700f36ab3c2ab107b984ec9f3b8be5f055
fetch_pinned date https://github.com/HowardHinnant/date.git f94b8f36c6180be0021876c4a397a054fe50c6f2
fetch_pinned utfcpp https://github.com/nemtrif/utfcpp.git 65701fe00700577f9f4e9b96d682bd7cfcc0b76e
fetch_pinned disruptorplus https://github.com/xenia-canary/disruptorplus.git bf9c70c0cc92b6435f7edec75e12751ea37e5c2f
fetch_pinned xbyak https://github.com/herumi/xbyak.git 4e44f4614ddbf038f2a6296f5b906d5c72691e0f
fetch_pinned xbyak_aarch64 https://github.com/fujitsu/xbyak_aarch64.git 59c6a7e7368cd2dbb07c46abe83239ace4d58849
# The presenter's sharpening and upscaling shaders.
fetch_pinned FidelityFX-CAS https://github.com/GPUOpen-Effects/FidelityFX-CAS.git 9fabcc9a2c45f958aff55ddfda337e74ef894b7f
fetch_pinned FidelityFX-FSR https://github.com/GPUOpen-Effects/FidelityFX-FSR.git a21ffb8f6c13233ba336352bdff293894c706575
# Headers only: Xenia loads SPIRV-Tools at run time, for optional validation.
fetch_pinned SPIRV-Tools https://github.com/KhronosGroup/SPIRV-Tools.git 33e02568181e3312f49a3cf33df470bf96ef293a
# SDL2 for the runtime's host side: audio output and game controllers (Linux, macOS, iOS).
SDL_COMMIT=c98c4fbff6d8f3016a3ce6685bf8f43433c3efcc # release-2.30.9
if [ ! -d "$BUILD/SDL/.git" ]; then
	git init -q "$BUILD/SDL"
	git -C "$BUILD/SDL" remote add origin https://github.com/libsdl-org/SDL.git
fi
if [ "$(git -C "$BUILD/SDL" rev-parse -q --verify HEAD 2>/dev/null)" != "$SDL_COMMIT" ]; then
	git -C "$BUILD/SDL" fetch -q --depth 1 origin "$SDL_COMMIT"
	git -C "$BUILD/SDL" checkout -q FETCH_HEAD
fi

# fmt and xxhash: the copies XenonRecomp already brings, so the runtime links one of each.
# (Xenia's checkout can leave empty directories for its own copies, which ln would link into.)
for lib in fmt:fmt xxHash:xxhash; do
	link="$XENIA/third_party/${lib#*:}"
	[ -L "$link" ] || [ ! -d "$link" ] || rmdir "$link"
	ln -sfn "$BUILD/XenonRecomp/thirdparty/${lib%%:*}" "$link"
done

for patch in "$HERE"/patches/xenia/*.patch; do
	[ -e "$patch" ] || continue
	if git -C "$XENIA" apply --reverse --check "$patch" 2>/dev/null; then
		echo "already applied: xenia/$(basename "$patch")"
	else
		git -C "$XENIA" apply "$patch"
	fi
done

# Xenia's built-in shaders, as its build generates them.
python3 "$HERE/tools/build_xenia_shaders.py" "$XENIA"

echo "toolchain ready in $BUILD"
