#!/usr/bin/env bash
# Downloads the MoltenVK release for iOS and lays it out as
#   $MOLTENVK_DIR/include   (vulkan/, vk_video/, MoltenVK/ headers)
#   $MOLTENVK_DIR/lib/libMoltenVK.a   (static, ios-arm64)
set -euo pipefail
source "$(dirname "$0")/common.sh"

if [ -f "$MOLTENVK_DIR/lib/libMoltenVK.a" ]; then
	echo "MoltenVK already present in $MOLTENVK_DIR"
	exit 0
fi

TMP="$BUILD/moltenvk-download"
rm -rf "$TMP"
mkdir -p "$TMP" "$MOLTENVK_DIR/lib"

url="https://github.com/KhronosGroup/MoltenVK/releases/download/$MOLTENVK_VERSION/MoltenVK-ios.tar"
log "MoltenVK $MOLTENVK_VERSION from $url"
curl -fL --retry 4 -o "$TMP/MoltenVK-ios.tar" "$url"
tar -xf "$TMP/MoltenVK-ios.tar" -C "$TMP"

lib="$(find "$TMP" -path '*static*' -path '*ios-arm64/*' -name 'libMoltenVK.a' | grep -v simulator | head -n1)"
if [ -z "$lib" ]; then
	echo "libMoltenVK.a for ios-arm64 not found in the release archive:" >&2
	find "$TMP" -maxdepth 6 | head -n 80 >&2
	exit 1
fi
cp "$lib" "$MOLTENVK_DIR/lib/libMoltenVK.a"

inc="$(dirname "$(find "$TMP" -path '*/include/vulkan/vulkan.h' | head -n1)")/.."
if [ ! -f "$inc/MoltenVK/mvk_vulkan.h" ]; then
	echo "MoltenVK headers not found next to vulkan/vulkan.h ($inc)" >&2
	exit 1
fi
rm -rf "$MOLTENVK_DIR/include"
cp -R "$inc" "$MOLTENVK_DIR/include"

log "MoltenVK ready in $MOLTENVK_DIR"
ls "$MOLTENVK_DIR/include"
