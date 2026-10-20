# Bayonetta (Xbox 360) native runtime. GPL-3.0, see recomp/COPYING.
cmake_minimum_required(VERSION 3.20)
project(BayonettaRecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
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

add_executable(bayonetta
    src/main.cpp
    src/apu/audio.cpp
    src/hid/hid.cpp
    src/cpu/guest_thread.cpp
    src/kernel/heap.cpp
    src/kernel/imports.cpp
    src/kernel/imports_bayonetta.cpp
    src/kernel/crt.cpp
    src/kernel/memory.cpp
    src/kernel/module.cpp
    src/kernel/rtl.cpp
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
# UnleashedRecomp's sources assume stdafx.h is always included first (it is their PCH).
target_compile_options(bayonetta PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/stdafx.h>")
target_link_libraries(bayonetta PRIVATE BayonettaPPC XenonUtils fmt::fmt xxHash::xxhash Threads::Threads)
