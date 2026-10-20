// Ips3 shim for Xenia's xenia/kernel/kernel_state.h: what the GPU asks the kernel for. Graphics
// interrupts go to the runtime through ips3_dispatch_gpu_interrupt (gpu/xenia_bridge.cpp).
#pragma once

#include <cstdint>

#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"

namespace xe {
class Emulator;
class Memory;
namespace cpu {
class Processor;
}

namespace kernel {

constexpr uint16_t XCONFIG_USER_CATEGORY = 0x0003;
constexpr uint16_t XCONFIG_USER_AV_COMPOSITE_SCREENSZ = 0x000E;

// Screen size as XConfig packs it: width in the high half, height in the low half.
struct Resolution {
  explicit Resolution(uint32_t packed) : width_(packed >> 16), height_(packed & 0xFFFF) {}
  uint32_t width_;
  uint32_t height_;
};

class XConfig {
 public:
  template <typename T>
  T ReadSetting(uint16_t, uint16_t) const {
    return T((1280u << 16) | 720u);
  }
};

class KernelState {
 public:
  KernelState(Emulator* emulator, Memory* memory, cpu::Processor* processor)
      : emulator_(emulator), memory_(memory), processor_(processor) {}

  Emulator* emulator() const { return emulator_; }
  Memory* memory() const { return memory_; }
  cpu::Processor* processor() const { return processor_; }
  uint32_t GetIdleProcess() const { return 0; }
  bool is_title_open() const { return true; }
  XConfig* xconfig() { return &xconfig_; }
  UserModule* GetExecutableModule() { return &module_; }

  // Runs the guest graphics interrupt handler.
  void EmulateCPInterruptDPC(uint32_t interrupt_callback, uint32_t interrupt_callback_data,
                             uint32_t source, uint32_t cpu);

 private:
  Emulator* emulator_;
  Memory* memory_;
  cpu::Processor* processor_;
  XConfig xconfig_;
  UserModule module_;
};

}  // namespace kernel
}  // namespace xe

// Implemented by the Ips3 runtime.
void ips3_dispatch_gpu_interrupt(uint32_t callback, uint32_t data, uint32_t source, uint32_t cpu);
