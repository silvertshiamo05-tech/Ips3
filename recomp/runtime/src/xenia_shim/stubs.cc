// Ips3: Xenia functions the GPU code links against that have no job in the runtime.

#include <cstdio>

#include "xenia/base/system.h"
#include "xenia/config.h"
#include "xenia/memory.h"
#include "xenia/ui/window.h"

namespace xe {

void ShowSimpleMessageBox(SimpleMessageBoxType, std::string_view message) {
  std::fprintf(stderr, "[gpu] %.*s\n", int(message.size()), message.data());
}

// Physical memory is all mapped read/write in the runtime: the placeholder heap that
// Memory::GetPhysicalHeap returns reports every range as committed and writable.
xe::memory::PageAccess BaseHeap::QueryRangeAccess(uint32_t, uint32_t) {
  return xe::memory::PageAccess::kReadWrite;
}

bool BaseHeap::QueryRegionInfo(uint32_t, HeapAllocationInfo* out_info) {
  *out_info = {};
  out_info->base_address = 0;
  out_info->allocation_base = 0;
  out_info->allocation_protect = kMemoryProtectRead | kMemoryProtectWrite;
  out_info->allocation_size = 0x20000000;
  out_info->region_size = 0x20000000;
  out_info->state = kMemoryAllocationReserve | kMemoryAllocationCommit;
  out_info->protect = kMemoryProtectRead | kMemoryProtectWrite;
  return true;
}

namespace ui {
// The runtime presents offscreen; there is no window.
void Window::SetPresenter(Presenter*) {}
}  // namespace ui

}  // namespace xe

namespace config {
void SaveConfig() {}
}  // namespace config

#if XE_PLATFORM_ANDROID
#include <android/api-level.h>

#include "xenia/base/main_android.h"

// Xenia's Android app setup (main_android.cc) is not used: SDL's activity runs the runtime.
namespace xe {
int32_t GetAndroidApiLevel() { return android_get_device_api_level(); }
JNIEnv* GetAndroidThreadJniEnv() { return nullptr; }
jobject GetAndroidApplicationContext() { return nullptr; }
}  // namespace xe
#endif
