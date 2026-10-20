# Ips3: the iOS core, as Ips3Core.framework.
#
# Added from the ARMSX3 root CMakeLists behind `if(IOS)` (see patches/armsx3), after
# add_subdirectory(rpcs3), so rpcs3_emu already exists. This mirrors android/CMakeLists.txt:
# rpcs3/Input and rpcs3_version.cpp are compiled into the rpcs3 executable upstream, which
# is not built here, so the frontend library has to build them itself.

set(IPS3_RPCS3_DIR ${CMAKE_SOURCE_DIR}/rpcs3)

# The platform-safe subset of rpcs3/Input, as on Android. Excluded: the Qt/desktop backends
# (keyboard, raw mouse, SDL, evdev, XInput, MMJoystick) and the Android IME keyboard.
set(IPS3_INPUT_SOURCES
    ${IPS3_RPCS3_DIR}/Input/pad_thread.cpp
    ${IPS3_RPCS3_DIR}/Input/product_info.cpp
    ${IPS3_RPCS3_DIR}/Input/hid_pad_handler.cpp
    ${IPS3_RPCS3_DIR}/Input/ds3_pad_handler.cpp
    ${IPS3_RPCS3_DIR}/Input/ds4_pad_handler.cpp
    ${IPS3_RPCS3_DIR}/Input/dualsense_pad_handler.cpp
    ${IPS3_RPCS3_DIR}/Input/skateboard_pad_handler.cpp
    ${IPS3_RPCS3_DIR}/Input/ps_move_handler.cpp
    ${IPS3_RPCS3_DIR}/Input/ps_move_config.cpp
    ${IPS3_RPCS3_DIR}/Input/ps_move_calibration.cpp
    ${IPS3_RPCS3_DIR}/Input/ps_move_tracker.cpp
    ${IPS3_RPCS3_DIR}/Input/mouse_gyro_handler.cpp
    ${IPS3_RPCS3_DIR}/Input/virtual_pad_handler.cpp
)

add_library(Ips3Core SHARED
    src/ips3_core.cpp
    src/CoreAudioBackend.cpp
    include/ips3_core.h
    ${IPS3_INPUT_SOURCES}
    ${IPS3_RPCS3_DIR}/rpcs3_version.cpp
)

set_target_properties(Ips3Core PROPERTIES
    FRAMEWORK TRUE
    FRAMEWORK_VERSION A
    MACOSX_FRAMEWORK_IDENTIFIER com.ips3.core
    MACOSX_FRAMEWORK_BUNDLE_VERSION 1
    MACOSX_FRAMEWORK_SHORT_VERSION_STRING 1.0
    PUBLIC_HEADER include/ips3_core.h
    # Loaded by path with dlopen() from the app's Frameworks directory.
    INSTALL_NAME_DIR "@rpath"
    BUILD_WITH_INSTALL_RPATH TRUE
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN TRUE
)

target_compile_features(Ips3Core PRIVATE cxx_std_23)

target_include_directories(Ips3Core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}
    ${IPS3_RPCS3_DIR}
    ${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include
)

target_link_libraries(Ips3Core PRIVATE
    rpcs3_emu
    3rdparty::fusion
    3rdparty::hidapi
    3rdparty::libusb
    "-framework Foundation"
    "-framework UIKit"
    "-framework QuartzCore"
    "-framework Metal"
    "-framework IOSurface"
    "-framework CoreGraphics"
    "-framework AVFoundation"
    "-framework AudioToolbox"
    "-framework CoreAudio"
    "-framework CoreMIDI"
    "-framework Security"
    "-framework SystemConfiguration"
    "-framework CFNetwork"
)

# Symbols stay exported: LLVM's JIT may resolve helpers by name at runtime.
target_link_options(Ips3Core PRIVATE -Wl,-dead_strip)
