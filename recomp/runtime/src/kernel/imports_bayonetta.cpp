// Ips3: kernel imports Bayonetta uses that Sonic Unleashed does not.
#include <stdafx.h>

#include <cpu/guest_thread.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/xdm.h>

static uint32_t KeSetDisableBoostThread(uint32_t thread, uint32_t disable)
{
	// Priority boosting is the host scheduler's business.
	return 0;
}

static uint32_t MmMapIoSpace(uint32_t physicalAddress, uint32_t size, uint32_t protect)
{
	// The game maps the GPU's register block this way. Guest memory is flat, so the address
	// maps to itself; register writes land in memory for the renderer to pick up.
	LOGF_UTILITY("physical {:08X}, size {:X} -- GPU registers are plain memory for now", physicalAddress, size);
	return physicalAddress;
}

static uint32_t NtYieldExecution()
{
	std::this_thread::yield();
	return 0;
}

static uint32_t ObLookupThreadByThreadId(uint32_t threadId, be<uint32_t>* thread)
{
	LOGF_UTILITY("thread {:X}", threadId);
	*thread = 0;
	return 0xC0000008; // STATUS_INVALID_HANDLE
}

static uint32_t ObOpenObjectByPointer(uint32_t object, be<uint32_t>* handle)
{
	LOGF_UTILITY("object {:08X}", object);
	*handle = 0;
	return 0xC0000008; // STATUS_INVALID_HANDLE
}

static uint32_t XAudioGetSpeakerConfig(be<uint32_t>* config)
{
	*config = 0x00010001; // stereo, as Xenia reports
	return 0;
}

GUEST_FUNCTION_HOOK(__imp__KeSetDisableBoostThread, KeSetDisableBoostThread);
GUEST_FUNCTION_HOOK(__imp__MmMapIoSpace, MmMapIoSpace);
GUEST_FUNCTION_HOOK(__imp__NtYieldExecution, NtYieldExecution);
GUEST_FUNCTION_HOOK(__imp__ObLookupThreadByThreadId, ObLookupThreadByThreadId);
GUEST_FUNCTION_HOOK(__imp__ObOpenObjectByPointer, ObOpenObjectByPointer);
GUEST_FUNCTION_HOOK(__imp__XAudioGetSpeakerConfig, XAudioGetSpeakerConfig);
