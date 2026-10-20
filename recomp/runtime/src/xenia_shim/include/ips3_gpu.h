// Ips3: the runtime's interface to Xenia's GPU (implemented in xenia_shim/gpu_host.cc). Kept
// free of Xenia and runtime headers so both sides can include it.
#pragma once

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
bool ips3_gpu_capture_frame(const char* path);

// Runtime side.
void ips3_dispatch_gpu_interrupt(uint32_t callback, uint32_t data, uint32_t source, uint32_t cpu);
// Before a system call writes guest memory (watched pages would make it fail).
void ips3_memory_will_write(uint32_t guest_address, uint32_t length);
