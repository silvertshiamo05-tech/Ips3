// Ips3 shim for Xenia's xenia/emulator.h.
#pragma once

#include "xenia/apu/audio_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {
class GraphicsSystem;
}

class Emulator {
 public:
  apu::AudioSystem* audio_system() { return nullptr; }
  gpu::GraphicsSystem* graphics_system() const { return graphics_system_; }
  Memory* memory() const { return memory_; }

  gpu::GraphicsSystem* graphics_system_ = nullptr;
  Memory* memory_ = nullptr;
};

}  // namespace xe
