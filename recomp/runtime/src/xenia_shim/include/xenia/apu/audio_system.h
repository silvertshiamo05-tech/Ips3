// Ips3 shim for Xenia's xenia/apu/audio_system.h (the GPU only asks for its name).
#pragma once

#include <string>

namespace xe {
namespace apu {

class AudioSystem {
 public:
  std::string name() const { return "none"; }
};

}  // namespace apu
}  // namespace xe
