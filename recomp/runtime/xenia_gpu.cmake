# Xenia's GPU emulation and Vulkan backend (BSD-3-Clause, fetched by recomp/setup.sh into
# recomp/build/xenia), built against the Ips3 runtime. src/xenia_shim provides the parts of
# Xenia's emulator, kernel and CPU the GPU code expects; they forward to the runtime.

set(XENIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/xenia")
set(XENIA_SRC "${XENIA_DIR}/src/xenia")
set(XENIA_SHIM "${CMAKE_CURRENT_SOURCE_DIR}/src/xenia_shim")

# The target CPU: CMAKE_SYSTEM_PROCESSOR, or for Apple cross builds (iOS) CMAKE_OSX_ARCHITECTURES.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(IPS3_ARM64 ON)
else()
    set(IPS3_ARM64 OFF)
endif()

if(IPS3_ARM64)
    # platform_arm64 detects CPU features with xbyak_aarch64's helper (pinned by setup.sh).
    set(XENIA_ARCH_SOURCES ${XENIA_SRC}/base/clock_arm64.cc ${XENIA_SRC}/base/platform_arm64.cc
        ${XENIA_DIR}/third_party/xbyak_aarch64/src/util_impl.cpp)
else()
    set(XENIA_ARCH_SOURCES ${XENIA_SRC}/base/clock_x64.cc ${XENIA_SRC}/base/platform_amd64.cc)
endif()

set(XENIA_BASE_SOURCES
    assert bit_map byte_stream clock clock_posix console_posix cvar debugging_posix exception_handler
    exception_handler_posix filesystem filesystem_posix filesystem_wildcard logging
    mapped_memory_posix memory memory_posix mutex profiling ring_buffer string string_buffer
    threading threading_posix threading_timer_queue utf8 vec128)
list(TRANSFORM XENIA_BASE_SOURCES PREPEND "${XENIA_SRC}/base/")
list(TRANSFORM XENIA_BASE_SOURCES APPEND ".cc")

file(GLOB XENIA_GPU_SOURCES ${XENIA_SRC}/gpu/*.cc ${XENIA_SRC}/gpu/vulkan/*.cc)
list(FILTER XENIA_GPU_SOURCES EXCLUDE REGEX "/(dxbc_[a-z_]*|shader_compiler_main|trace_dump|trace_viewer|trace_player|trace_reader|vulkan_trace_dump_main|vulkan_trace_viewer_main)\\.cc$")

file(GLOB XENIA_UI_SOURCES ${XENIA_SRC}/ui/vulkan/*.cc)
list(FILTER XENIA_UI_SOURCES EXCLUDE REGEX "/vulkan_window_demo\\.cc$")
list(APPEND XENIA_UI_SOURCES ${XENIA_SRC}/ui/presenter.cc ${XENIA_SRC}/ui/graphics_util.cc
    ${XENIA_SRC}/ui/graphics_upload_buffer_pool.cc ${XENIA_SRC}/ui/immediate_drawer.cc
    ${XENIA_SRC}/ui/renderdoc_api.cc)

set(GLSLANG_DIR "${XENIA_DIR}/third_party/glslang")
set(XENIA_GLSLANG_SOURCES ${GLSLANG_DIR}/SPIRV/SpvBuilder.cpp ${GLSLANG_DIR}/SPIRV/InReadableOrder.cpp
    ${GLSLANG_DIR}/SPIRV/Logger.cpp)

file(GLOB XENIA_SHIM_SOURCES ${XENIA_SHIM}/*.cc)

add_library(XeniaGpu STATIC ${XENIA_BASE_SOURCES} ${XENIA_ARCH_SOURCES} ${XENIA_GPU_SOURCES}
    ${XENIA_UI_SOURCES} ${XENIA_GLSLANG_SOURCES} ${XENIA_SHIM_SOURCES})

# The shim directory comes first so its xenia/emulator.h, xenia/kernel/... win.
target_include_directories(XeniaGpu PUBLIC
    ${XENIA_SHIM}/include
    ${XENIA_DIR}/src
    ${XENIA_DIR}
    ${XENIA_DIR}/third_party/Vulkan-Headers/include
    ${XENIA_DIR}/third_party/VulkanMemoryAllocator/include
    ${GLSLANG_DIR}
    ${XENIA_DIR}/third_party/fmt/include
    ${XENIA_DIR}/third_party/date/include)
target_compile_definitions(XeniaGpu PUBLIC XE_UI_VULKAN_FINE_GRAINED_DRAW_SCOPES=0 VK_NO_PROTOTYPES VULKAN_HPP_NO_EXCEPTIONS)
target_compile_options(XeniaGpu PRIVATE -w -fno-strict-aliasing)
if(IPS3_ARM64)
    target_include_directories(XeniaGpu PRIVATE ${XENIA_DIR}/third_party/xbyak_aarch64/xbyak_aarch64)
endif()
if(NOT IPS3_ARM64)
    # Xenia's x86 code assumes AVX-era CPUs (SSSE3 shuffles in the byte swappers and more).
    target_compile_options(XeniaGpu PRIVATE -march=x86-64-v3)
endif()
target_link_libraries(XeniaGpu PUBLIC fmt::fmt xxHash::xxhash Threads::Threads ${CMAKE_DL_LIBS})
