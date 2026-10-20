// Ips3: Xenos GPU command processor.
//
// The game's Direct3D writes PM4 packets into a ring buffer and bumps CP_RB_WPTR through the GPU
// register window at 0x7FC80000, which here is plain guest memory. This thread reads the ring,
// keeps the register file, performs the packets that talk back to the CPU (fences, memory
// writes, scratch registers, waits, interrupts) and writes the read pointer back. Drawing
// packets are decoded and counted but not rendered yet.
#pragma once

#include <cstdint>

namespace Gpu
{
	constexpr uint32_t REGISTER_BASE = 0x7FC80000;
	constexpr uint32_t REGISTER_WINDOW = 0x20000; // bytes

	// Guest address of a GPU (physical) address.
	inline uint32_t Translate(uint32_t gpuAddress)
	{
		return 0xA0000000 | (gpuAddress & 0x1FFFFFFF);
	}

	void InitializeRingBuffer(uint32_t gpuAddress, uint32_t sizeLog2);
	void EnableReadPointerWriteBack(uint32_t gpuAddress, uint32_t blockLog2);
	void SetInterruptCallback(uint32_t callback, uint32_t userData);

	// Frames the game has presented (VdSwap reached the command processor).
	uint64_t SwapCount();
}
