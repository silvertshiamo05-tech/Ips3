// Ips3: the runtime's interface to Xenia's GPU (implemented in xenia_shim/gpu_host.cc). Kept
// free of Xenia and runtime headers so both sides can include it.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include <vector>

// The 0xE0000000 view of physical memory is shifted by 4 KiB on the console (guest 0xE0000000
// is physical 0x1000). That can only be reproduced with 4 KiB host pages; on 16 KiB-page hosts
// (Apple silicon, iOS) the view is mapped unshifted and every conversion uses this instead.
inline uint32_t ips3_e_view_shift() {
  // IPS3_E_SHIFT=0 forces the unshifted layout of large-page hosts, to test it on 4 KB ones.
  static const uint32_t shift = getenv("IPS3_E_SHIFT") ? uint32_t(strtoul(getenv("IPS3_E_SHIFT"), nullptr, 0))
                                : sysconf(_SC_PAGESIZE) <= 0x1000 ? 0x1000 : 0;
  return shift;
}

#include <cstdint>

// Starts the Vulkan GPU over guest memory at guest_membase.
bool ips3_gpu_start(uint8_t* guest_membase);
// Before ips3_gpu_start: render at 1x or 2x the console's resolution.
void ips3_gpu_set_resolution_scale(uint32_t scale);
// Stops and restarts Xenia's command processor (iOS forbids GPU work in the background).
void ips3_gpu_set_paused(bool paused);

void ips3_gpu_initialize_ring_buffer(uint32_t gpu_address, uint32_t size_log2);
void ips3_gpu_enable_read_pointer_writeback(uint32_t gpu_address, uint32_t block_size_log2);
void ips3_gpu_set_interrupt_callback(uint32_t callback, uint32_t user_data);

// VdSwap: fills the 64 dwords the caller reserved in the ring (big-endian guest memory) from
// the front buffer's texture fetch constant (6 big-endian dwords).
void ips3_gpu_write_swap(uint32_t* buffer, const uint32_t* fetch, uint32_t width, uint32_t height);
uint64_t ips3_gpu_swap_count();
// Total time Xenia waited for the GPU to finish submitted work, and how many waits.
void ips3_gpu_wait_stats(uint64_t* wait_ns, uint64_t* waits);
// The command ring: where the game has written to, and where the GPU has read to.
void ips3_gpu_ring_state(uint32_t* write_pointer, uint32_t* read_pointer);
// Xenia's command processor: where it has read the ring to, PM4 packets executed, the last
// one's opcode, and whether it is executing commands right now.
void ips3_gpu_cp_state(uint32_t* read_pointer, uint64_t* packets, uint32_t* last_opcode, bool* executing);
// Graphics interrupts delivered to the game: vblank (source 0) and command processor (1).
void ips3_interrupt_counts(uint64_t* vblank, uint64_t* command_processor);
// Write watches armed on guest memory the GPU holds copies of, and how many fired.
void ips3_watch_stats(uint64_t* arms, uint64_t* triggers);

// Saves the last presented frame as a PNG.
// Reads back the latest guest output as tightly packed R8G8B8X8 pixels (for host windows).
bool ips3_gpu_grab_frame(std::vector<uint8_t>& rgbx, uint32_t& width, uint32_t& height);
bool ips3_gpu_capture_frame(const char* path);

// Runtime side.
void ips3_dispatch_gpu_interrupt(uint32_t callback, uint32_t data, uint32_t source, uint32_t cpu);
// Before a system call writes guest memory (watched pages would make it fail).
void ips3_memory_will_write(uint32_t guest_address, uint32_t length);
