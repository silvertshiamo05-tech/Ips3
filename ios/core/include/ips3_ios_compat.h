// Force-included into every C++/Objective-C++ translation unit of the iOS build.
//
// RPCS3's Apple path toggles MAP_JIT write protection per thread with
// pthread_jit_write_protect_np(). That API is macOS-only (the iOS SDK declares it
// unavailable) and iOS has no MAP_JIT for us anyway: JIT pages are W^X and flipped on
// fault instead (rpcs3/util/vm_native.cpp, ios_jit_handle_fault). The calls become no-ops.
//
// <pthread.h> is included first so its own declaration is parsed before the macro exists.
#pragma once

#ifdef __cplusplus
#include <pthread.h>
// asmjit only includes this for macOS.
#include <libkern/OSCacheControl.h>

#define pthread_jit_write_protect_np(enabled) (static_cast<void>(enabled))
#define pthread_jit_write_protect_supported_np() 0
#endif
