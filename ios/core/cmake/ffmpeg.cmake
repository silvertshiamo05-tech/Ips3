# Ips3: prebuilt FFmpeg for iOS arm64, built by scripts/build-ffmpeg-ios.sh.
#
# Included from the ARMSX3 root CMakeLists *before* add_subdirectory(3rdparty): upstream
# skips its own FFmpeg block on iOS (patched to `if(NOT ANDROID AND NOT IOS)`) but still
# aliases 3rdparty::ffmpeg -> 3rdparty_ffmpeg, so the target has to exist by then. This is
# the same arrangement android/ffmpeg.cmake uses.

if(NOT FFMPEG_IOS_DIR)
    message(FATAL_ERROR "Set -DFFMPEG_IOS_DIR=<prefix> to the FFmpeg iOS install (scripts/build-ffmpeg-ios.sh)")
endif()

foreach(lib avformat avcodec avutil swscale swresample)
    if(NOT EXISTS "${FFMPEG_IOS_DIR}/lib/lib${lib}.a")
        message(FATAL_ERROR "Ips3: ${FFMPEG_IOS_DIR}/lib/lib${lib}.a is missing")
    endif()

    add_library(ffmpeg::${lib} STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg::${lib} PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/lib/lib${lib}.a"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_IOS_DIR}/include")
endforeach()

add_library(3rdparty_ffmpeg INTERFACE)
target_link_libraries(3rdparty_ffmpeg INTERFACE
    ffmpeg::avformat
    ffmpeg::avcodec
    ffmpeg::swscale
    ffmpeg::swresample
    ffmpeg::avutil
    # What FFmpeg's own configure links on Darwin for the components it enables.
    "-framework CoreFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework Security"
    "-framework VideoToolbox"
    "-framework AudioToolbox"
    z
    bz2
    iconv
)
