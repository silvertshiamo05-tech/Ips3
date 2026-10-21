// Ips3: the runtime's interface to Xenia's GPU (implemented in xenia_shim/gpu_host.cc). Kept
// free of Xenia and runtime headers so both sides can include it.
#pragma once

#include <cstdint>
#include <unistd.h>
#include <vector>

// The 0xE0000000 view of physical memory is shifted by 4 KiB on the console (guest 0xE0000000
// is physical 0x1000). That can only be reproduced with 4 KiB host pages; on 16 KiB-page hosts
// (Apple silicon, iOS) the view is mapped unshifted and every conversion uses this instead.
inline uint32_t ips3_e_view_shift() {
  static const uint32_t shift = sysconf(_SC_PAGESIZE) <= 0x1000 ? 0x1000 : 0;
  return shift;
}

#include <cstdint>

// Starts the Vulkan GPU over guest memory at guest_membase.
bool ips3_gpu_start(uint8_t* guest_membase);

void ips3_gpu_initialize_ring_buffer(uint32_t gpu_address, uint32_t size_log2);
void ips3_gpu_enable_read_pointer_writeback(uint32_t gpu_address, uint32_t block_size_log2);
void ips3_gpu_set_interrupt_callback(uint32_t callback, uint32_t user_data);

// VdSwap: fills the 64 dwords the caller reserved in the ring (big-endian guest memory) from
// the front buffer's texture fetch constant (6 big-endian dwords).
void ips3_gpu_write_swap(uint32_t* buffer, const uint32_t* fetch, uint32_t width, uint32_t height);
uint64_t ips3_gpu_swap_count();

// Saves the last presented frame as a PNG.
// Reads back the latest guest output as tightly packed R8G8B8X8 pixels (for host windows).
bool ips3_gpu_grab_frame(std::vector<uint8_t>& rgbx, uint32_t& width, uint32_t& height);
bool ips3_gpu_capture_frame(const char* path);

// Runtime side.
void ips3_dispatch_gpu_interrupt(uint32_t callback, uint32_t data, uint32_t source, uint32_t cpu);
// Before a system call writes guest memory (watched pages would make it fail).
void ips3_memory_will_write(uint32_t guest_address, uint32_t length);
