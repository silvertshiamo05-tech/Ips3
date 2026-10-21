# Bayonetta (Xbox 360) native runtime. GPL-3.0, see recomp/COPYING.
cmake_minimum_required(VERSION 3.20)
# On iOS the host side has Objective-C++ (the file picker); CMake needs it enabled up front.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    project(BayonettaRecomp LANGUAGES C CXX OBJC OBJCXX)
else()
    project(BayonettaRecomp LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
if(ANDROID)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON) # everything goes into one shared library
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XENONRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/XenonRecomp" CACHE PATH "Patched XenonRecomp checkout")
set(PPC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bayonetta/private/ppc" CACHE PATH "Recompiled code (private, never committed)")

# XenonUtils (XEX loader) and its third-party libraries (fmt, xxHash, ...).
add_subdirectory(${XENONRECOMP_DIR} ${CMAKE_BINARY_DIR}/xenonrecomp EXCLUDE_FROM_ALL)

# The recompiled game: ~300 generated files.
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${PPC_DIR}/ppc_recomp.*.cpp)
add_library(BayonettaPPC STATIC ${PPC_SOURCES} ${PPC_DIR}/ppc_func_mapping.cpp)
target_include_directories(BayonettaPPC PUBLIC
    ${PPC_DIR}/..
    ${PPC_DIR}
    ${XENONRECOMP_DIR}/thirdparty/simde)
# Generated code is huge and machine-written: optimise it, but skip the warnings.
target_compile_options(BayonettaPPC PRIVATE -w -fno-strict-aliasing)
# Report indirect calls into code with no recompiled function instead of jumping to 0.
target_compile_options(BayonettaPPC PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/indirect_call.h")

# Android loads the runtime as libmain.so from SDL's Java activity; elsewhere it is a program.
if(ANDROID)
    add_library(bayonetta SHARED)
    set_target_properties(bayonetta PROPERTIES OUTPUT_NAME main)
else()
    add_executable(bayonetta)
endif()
target_sources(bayonetta PRIVATE
    src/main.cpp
    src/apu/audio.cpp
    src/hid/hid.cpp
    src/host/host_sdl.cpp
    src/host/host_window.cpp
    src/cpu/guest_thread.cpp
    src/gpu/interrupts.cpp
    src/kernel/heap.cpp
    src/kernel/imports.cpp
    src/kernel/imports_bayonetta.cpp
    src/kernel/crt.cpp
    src/kernel/memory.cpp
    src/kernel/module.cpp
    src/kernel/rtl.cpp
    src/kernel/video.cpp
    src/kernel/virtual_memory.cpp
    src/kernel/xam.cpp
    src/kernel/xdm.cpp
    src/kernel/io/file_system.cpp
    src/kernel/io/disc_image.cpp
    src/kernel/io/nt_file.cpp
    thirdparty/o1heap/o1heap.c)

target_include_directories(bayonetta PRIVATE
    src
    src/kernel
    thirdparty/o1heap
    thirdparty/unordered_dense/include
    ${XENONRECOMP_DIR}/XenonUtils
    ${XENONRECOMP_DIR}/thirdparty/xxHash
    ${XENONRECOMP_DIR}/thirdparty/fmt/include)

find_package(Threads REQUIRED)

# SDL2 (pinned by setup.sh), static: audio output and game controllers.
set(SDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/SDL" CACHE PATH "SDL2 checkout")
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} ${CMAKE_BINARY_DIR}/sdl EXCLUDE_FROM_ALL)

include(xenia_gpu.cmake)
# UnleashedRecomp's sources assume stdafx.h is always included first (it is their PCH).
target_compile_options(bayonetta PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/stdafx.h>")
target_link_libraries(bayonetta PRIVATE BayonettaPPC XeniaGpu XenonUtils fmt::fmt xxHash::xxhash Threads::Threads SDL2::SDL2-static)
set_target_properties(bayonetta PROPERTIES ENABLE_EXPORTS ON) # function names in crash backtraces
if(TARGET SDL2::SDL2main)
    target_link_libraries(bayonetta PRIVATE SDL2::SDL2main) # provides main() on iOS
endif()

if(ANDROID)
    # The Android app (android/): SDL's activity plus the disc image picker and log location.
    target_sources(bayonetta PRIVATE src/host/android_files.cpp)
    target_link_libraries(bayonetta PRIVATE android log)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # The iOS app: Bayonetta.app with the runtime as its executable. Xenia's Vulkan backend runs
    # on MoltenVK, linked statically (MOLTENVK_LIB: libMoltenVK.a from the MoltenVK release).
    target_sources(bayonetta PRIVATE src/host/ios_files.mm)
    set(MOLTENVK_LIB "" CACHE FILEPATH "libMoltenVK.a for iOS (arm64)")
    if(NOT MOLTENVK_LIB)
        message(FATAL_ERROR "Set MOLTENVK_LIB to MoltenVK's static library for iOS")
    endif()
    target_link_libraries(bayonetta PRIVATE ${MOLTENVK_LIB}
        "-framework Metal" "-framework MetalKit" "-framework QuartzCore" "-framework IOSurface"
        "-framework UIKit" "-framework Foundation" "-framework CoreGraphics"
        "-framework UniformTypeIdentifiers" "-framework GameController" "-framework AVFoundation"
        "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreMotion" "-framework CoreHaptics")
    set_target_properties(bayonetta PROPERTIES
        OUTPUT_NAME Bayonetta
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER com.ips3.bayonetta
        MACOSX_BUNDLE_BUNDLE_NAME Bayonetta
        MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1
        MACOSX_BUNDLE_BUNDLE_VERSION 1)
endif()
