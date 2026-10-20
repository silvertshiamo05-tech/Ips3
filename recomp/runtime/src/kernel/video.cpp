// Ips3: the kernel's video functions (Vd*), backed by the headless command processor. Behaviour
// and structure layouts follow Xenia's xboxkrnl_video.
#include <stdafx.h>

#include <gpu/command_processor.h>
#include <kernel/function.h>
#include <kernel/memory.h>

namespace
{
	constexpr uint32_t DISPLAY_WIDTH = 1280;
	constexpr uint32_t DISPLAY_HEIGHT = 720;

	template<typename T>
	void Put(uint8_t* base, uint32_t offset, T value)
	{
		if constexpr (sizeof(T) == 1)
			base[offset] = uint8_t(value);
		else
			*reinterpret_cast<T*>(base + offset) = ByteSwap(value);
	}

	uint32_t FloatBits(float value)
	{
		uint32_t bits;
		memcpy(&bits, &value, sizeof(bits));
		return bits;
	}
}

// BOOL VdInitializeEngines(uint32_t unknown, void* callback, void* argument, void* pfp, void* me)
uint32_t VdInitializeEngines(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
	return 1;
}

void VdShutdownEngines()
{
}

// void VdSetGraphicsInterruptCallback(void (*callback)(uint32_t source, void* data), void* data)
void VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t data)
{
	Gpu::SetInterruptCallback(callback, data);
}

// void VdInitializeRingBuffer(uint32_t gpuAddress, uint32_t sizeLog2)
void VdInitializeRingBuffer(uint32_t gpuAddress, uint32_t sizeLog2)
{
	Gpu::InitializeRingBuffer(gpuAddress, sizeLog2);
}

// void VdEnableRingBufferRPtrWriteBack(uint32_t gpuAddress, uint32_t blockLog2)
void VdEnableRingBufferRPtrWriteBack(uint32_t gpuAddress, uint32_t blockLog2)
{
	Gpu::EnableReadPointerWriteBack(gpuAddress, blockLog2);
}

void VdSetSystemCommandBufferGpuIdentifierAddress(uint32_t)
{
}

// void VdGetSystemCommandBuffer(void** buffer, uint32_t* size)
void VdGetSystemCommandBuffer(be<uint32_t>* first, be<uint32_t>* second)
{
	if (first) *first = 0xBEEF0000;
	if (second) *second = 0xBEEF0001;
}

uint32_t VdIsHSIOTrainingSucceeded()
{
	return 1;
}

uint32_t VdRetrainEDRAMWorker(uint32_t)
{
	return 0;
}

uint32_t VdEnableDisableClockGating(uint32_t)
{
	return 0;
}

uint32_t VdSetDisplayMode(uint32_t)
{
	return 0;
}

uint32_t VdCallGraphicsNotificationRoutines(uint32_t, uint32_t)
{
	return 0;
}

// Widescreen, at least 1024 wide.
uint32_t VdQueryVideoFlags()
{
	return 0x3;
}

// void VdGetCurrentDisplayGamma(uint32_t* type, float* power)
void VdGetCurrentDisplayGamma(be<uint32_t>* type, be<uint32_t>* power)
{
	if (type) *type = 2;
	if (power) *power = FloatBits(2.22222233f);
}

// void VdGetCurrentDisplayInformation(DISPLAY_INFO* info), 0x58 bytes.
void VdGetCurrentDisplayInformation(uint8_t* info)
{
	memset(info, 0, 0x58);
	Put<uint16_t>(info, 0x00, DISPLAY_WIDTH);   // front buffer
	Put<uint16_t>(info, 0x02, DISPLAY_HEIGHT);
	Put<uint8_t>(info, 0x04, 6);                // color format
	Put<uint8_t>(info, 0x05, 0x1A);             // pixel format
	Put<uint32_t>(info, 0x08, 0);               // scaler source rect
	Put<uint32_t>(info, 0x0C, 0);
	Put<uint32_t>(info, 0x10, DISPLAY_WIDTH);
	Put<uint32_t>(info, 0x14, DISPLAY_HEIGHT);
	Put<uint32_t>(info, 0x18, DISPLAY_WIDTH);   // scaled output
	Put<uint32_t>(info, 0x1C, DISPLAY_HEIGHT);
	Put<uint16_t>(info, 0x40, DISPLAY_WIDTH);   // display
	Put<uint16_t>(info, 0x42, DISPLAY_HEIGHT);
	Put<uint32_t>(info, 0x44, FloatBits(60.0f));
	Put<uint32_t>(info, 0x48, 0);               // progressive
	Put<uint8_t>(info, 0x4C, 6);
	Put<uint16_t>(info, 0x4E, DISPLAY_WIDTH);
}

// uint32_t VdInitializeScalerCommandBuffer(8 x uint32_t, void*, void*, uint32_t* dest, uint32_t count)
// Bayonetta's kernel takes four stack arguments: two pointers, then the destination and its
// size in dwords. The caller fills the destination with no-ops beforehand; leave it as is and
// report it used in full.
uint32_t VdInitializeScalerCommandBuffer(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
	uint32_t, uint32_t, uint32_t dest, uint32_t count)
{
	return count;
}

// void VdSwap(uint32_t* buffer, void* fetch, void* writeback, void* sysBuffer, void* sysBuffer2,
//     uint32_t* frontBuffer, uint32_t* format, uint32_t* colorSpace, uint32_t* width, uint32_t* height)
// The caller reserves 64 dwords in the ring for this. They become one packet the command
// processor counts as a presented frame.
void VdSwap(be<uint32_t>* buffer, uint32_t fetch, uint32_t writeback, uint32_t sysBuffer, uint32_t sysBuffer2,
	be<uint32_t>* frontBuffer, be<uint32_t>* format, be<uint32_t>* colorSpace, be<uint32_t>* width, be<uint32_t>* height)
{
	for (int i = 0; i < 64; i++)
		buffer[i] = 0;

	buffer[0] = 0xC0000000u | (62u << 16) | (0x64u << 8); // type 3, opcode 0x64, 63 dwords
	buffer[1] = 0x53574150;                              // 'SWAP'
	buffer[2] = frontBuffer ? uint32_t(*frontBuffer) : 0;
	buffer[3] = width ? uint32_t(*width) : DISPLAY_WIDTH;
	buffer[4] = height ? uint32_t(*height) : DISPLAY_HEIGHT;
}

GUEST_FUNCTION_HOOK(__imp__VdInitializeEngines, VdInitializeEngines);
GUEST_FUNCTION_HOOK(__imp__VdShutdownEngines, VdShutdownEngines);
GUEST_FUNCTION_HOOK(__imp__VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback);
GUEST_FUNCTION_HOOK(__imp__VdInitializeRingBuffer, VdInitializeRingBuffer);
GUEST_FUNCTION_HOOK(__imp__VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack);
GUEST_FUNCTION_HOOK(__imp__VdSetSystemCommandBufferGpuIdentifierAddress, VdSetSystemCommandBufferGpuIdentifierAddress);
GUEST_FUNCTION_HOOK(__imp__VdGetSystemCommandBuffer, VdGetSystemCommandBuffer);
GUEST_FUNCTION_HOOK(__imp__VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceeded);
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAMWorker, VdRetrainEDRAMWorker);
GUEST_FUNCTION_HOOK(__imp__VdEnableDisableClockGating, VdEnableDisableClockGating);
GUEST_FUNCTION_HOOK(__imp__VdSetDisplayMode, VdSetDisplayMode);
GUEST_FUNCTION_HOOK(__imp__VdCallGraphicsNotificationRoutines, VdCallGraphicsNotificationRoutines);
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoFlags, VdQueryVideoFlags);
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma);
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayInformation, VdGetCurrentDisplayInformation);
GUEST_FUNCTION_HOOK(__imp__VdInitializeScalerCommandBuffer, VdInitializeScalerCommandBuffer);
GUEST_FUNCTION_HOOK(__imp__VdSwap, VdSwap);
