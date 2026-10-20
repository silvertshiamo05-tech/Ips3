#!/usr/bin/env bash
# Builds static FFmpeg for iOS arm64 into $FFMPEG_IOS_DIR.
#
# RPCS3 needs avcodec/avformat/avutil/swscale/swresample: cellVdec (H.264, MPEG-2),
# cellAdec (ATRAC3+, MP3, AAC...), cellJpgDec/cellPngDec helpers and video recording.
set -euo pipefail
source "$(dirname "$0")/common.sh"

SRC="$BUILD/ffmpeg-src"

log "FFmpeg $FFMPEG_VERSION -> $FFMPEG_IOS_DIR"

if [ ! -d "$SRC" ]; then
	git clone --depth 1 --branch "$FFMPEG_VERSION" https://github.com/FFmpeg/FFmpeg.git "$SRC"
fi

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC="$(xcrun --sdk iphoneos -f clang)"
FLAGS="-arch arm64 -miphoneos-version-min=$IOS_DEPLOYMENT_TARGET -isysroot $SDK"

cd "$SRC"
make distclean >/dev/null 2>&1 || true

./configure \
	--prefix="$FFMPEG_IOS_DIR" \
	--enable-cross-compile \
	--target-os=darwin \
	--arch=aarch64 \
	--cc="$CC" \
	--sysroot="$SDK" \
	--extra-cflags="$FLAGS" \
	--extra-ldflags="$FLAGS" \
	--enable-static \
	--disable-shared \
	--enable-pic \
	--disable-programs \
	--disable-doc \
	--disable-debug \
	--disable-avdevice \
	--disable-avfilter \
	--disable-postproc \
	--disable-network \
	--disable-sdl2 \
	--disable-xlib \
	--disable-libxcb \
	--disable-lzma \
	--disable-securetransport \
	--disable-videotoolbox \
	--disable-audiotoolbox \
	--disable-appkit \
	--disable-avfoundation \
	--disable-coreimage \
	--disable-metal

make -j"$JOBS"
make install

log "Installed FFmpeg to $FFMPEG_IOS_DIR"
ls "$FFMPEG_IOS_DIR/lib"
