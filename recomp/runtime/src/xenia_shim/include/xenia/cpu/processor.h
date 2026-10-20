// Ips3 shim for Xenia's xenia/cpu/processor.h: the GPU code asks the processor for memory and,
// when debug logging, for a guest stack trace (not available in a static recompilation).
#pragma once

#include <cstdint>

namespace xe {
class Memory;
namespace cpu {
namespace backend {

constexpr uint32_t MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES = 32;

struct GuestPseudoStackTrace {
  uint32_t count = 0;
  uint32_t truncated_flag = 0;
  uint32_t return_addrs[MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES] = {};
};

class Backend {
 public:
  bool PopulatePseudoStacktrace(GuestPseudoStackTrace*) { return false; }
};

}  // namespace backend

class Processor {
 public:
  explicit Processor(Memory* memory) : memory_(memory) {}
  Memory* memory() const { return memory_; }
  backend::Backend* backend() { return &backend_; }

 private:
  Memory* memory_;
  backend::Backend backend_;
};

}  // namespace cpu
}  // namespace xe
