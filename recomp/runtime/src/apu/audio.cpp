// XAudio render driver for the guest. Placeholder: accepts the game's audio client and
// discards its frames until the CoreAudio output is written.
#include <stdafx.h>

#include <kernel/function.h>
#include <kernel/memory.h>

static uint32_t XAudioRegisterRenderDriverClient(be<uint32_t>* callback, be<uint32_t>* driver)
{
	LOGF_UTILITY("callback {:08X}, context {:08X} (audio output not implemented yet)", uint32_t(callback[0]), uint32_t(callback[1]));
	*driver = 0x41550000; // any non-zero handle
	return 0;
}

static uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver)
{
	return 0;
}

static uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, void* samples)
{
	return 0;
}

GUEST_FUNCTION_HOOK(__imp__XAudioRegisterRenderDriverClient, XAudioRegisterRenderDriverClient);
GUEST_FUNCTION_HOOK(__imp__XAudioUnregisterRenderDriverClient, XAudioUnregisterRenderDriverClient);
GUEST_FUNCTION_HOOK(__imp__XAudioSubmitRenderDriverFrame, XAudioSubmitRenderDriverFrame);
