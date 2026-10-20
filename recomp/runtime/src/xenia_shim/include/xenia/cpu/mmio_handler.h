// Ips3 shim for Xenia's xenia/cpu/mmio_handler.h: only the callback types the GPU code and
// xenia/memory.h name. The register window is plain memory in the Ips3 runtime (see
// gpu/xenia_bridge.cpp), so nothing traps accesses.
#pragma once

#include <cstdint>

namespace xe {
namespace cpu {

typedef uint32_t (*MMIOReadCallback)(void* ppc_context, void* callback_context, uint32_t addr);
typedef void (*MMIOWriteCallback)(void* ppc_context, void* callback_context, uint32_t addr,
                                  uint32_t value);

typedef void (*MmioAccessRecordCallback)(void* context, void* host_insn_address);

// Xenia traps register accesses with this; the Ips3 runtime polls instead, so it is never
// created.
class MMIOHandler;

struct MMIORange {
  uint32_t address;
  uint32_t mask;
  uint32_t size;
  void* callback_context;
  MMIOReadCallback read;
  MMIOWriteCallback write;
};

}  // namespace cpu
}  // namespace xe

// The GPU register window the GPU registered (Memory::AddVirtualMappedRange); the runtime
// forwards CP_RB_WPTR writes to its write callback.
extern xe::cpu::MMIORange ips3_gpu_register_range;
