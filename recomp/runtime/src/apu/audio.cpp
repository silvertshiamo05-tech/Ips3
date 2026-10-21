// XAudio render driver for the guest.
//
// The game registers one callback; the system calls it every 256 samples (48 kHz, ~5.33 ms)
// with a pointer to a word holding the registered argument (as Xenia's AudioSystem does),
// and the game mixes a frame and hands it back through XAudioSubmitRenderDriverFrame (6 planar
// channels of 256 big-endian floats). The driver thread below keeps that cadence so the game's
// sound engine advances; the frames are counted but not played yet (host output comes later).
#include <stdafx.h>

#include <cpu/guest_thread.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>

namespace
{
	constexpr uint32_t DRIVER_HANDLE = 0x41550000;
	constexpr auto FRAME_PERIOD = std::chrono::nanoseconds(1'000'000'000LL * 256 / 48000);

	std::atomic<uint32_t> g_callback{ 0 };
	std::atomic<uint32_t> g_callbackArg{ 0 };
	std::atomic<bool> g_running{ false };
	std::atomic<uint64_t> g_framesSubmitted{ 0 };

	void DriverThread()
	{
		GuestThreadContext context(0);
		auto next = std::chrono::steady_clock::now();

		while (g_running)
		{
			next += FRAME_PERIOD;
			std::this_thread::sleep_until(next);

			const uint32_t callback = g_callback;
			if (callback == 0)
				continue;

			PPCContext& ctx = context.ppcContext;
			ctx.r3.u64 = g_callbackArg; // guest address of the wrapped argument
			g_memory.FindFunction(callback)(ctx, g_memory.base);

			static uint64_t calls = 0;
			static auto lastReport = std::chrono::steady_clock::now();
			calls++;
			if (getenv("IPS3_TRACE_AUDIO") && std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(5))
			{
				lastReport = std::chrono::steady_clock::now();
				fmt::println(stderr, "[audio] {} callbacks, {} frames submitted", calls, g_framesSubmitted.load());
			}

			// Fell far behind (e.g. a debugger pause): don't replay the missed frames.
			if (std::chrono::steady_clock::now() - next > std::chrono::milliseconds(100))
				next = std::chrono::steady_clock::now();
		}
	}
}

// uint32_t XAudioRegisterRenderDriverClient(const uint32_t* callback /* {function, argument} */,
//     uint32_t* driver)
static uint32_t XAudioRegisterRenderDriverClient(be<uint32_t>* callback, be<uint32_t>* driver)
{
	auto* wrapped = static_cast<be<uint32_t>*>(g_userHeap.Alloc(sizeof(uint32_t)));
	*wrapped = uint32_t(callback[1]);
	g_callbackArg = g_memory.MapVirtual(wrapped);
	g_callback = callback[0];
	*driver = DRIVER_HANDLE;

	if (!g_running.exchange(true))
		std::thread(DriverThread).detach();

	return 0;
}

static uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver)
{
	g_callback = 0;
	return 0;
}

static uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, void* samples)
{
	g_framesSubmitted++;
	return 0;
}

// uint32_t XAudioGetVoiceCategoryVolume(uint32_t category, float* volume): full volume.
static uint32_t XAudioGetVoiceCategoryVolume(uint32_t category, be<float>* volume)
{
	if (volume)
		*volume = 1.0f;
	return 0;
}

GUEST_FUNCTION_HOOK(__imp__XAudioRegisterRenderDriverClient, XAudioRegisterRenderDriverClient);
GUEST_FUNCTION_HOOK(__imp__XAudioUnregisterRenderDriverClient, XAudioUnregisterRenderDriverClient);
GUEST_FUNCTION_HOOK(__imp__XAudioSubmitRenderDriverFrame, XAudioSubmitRenderDriverFrame);
GUEST_FUNCTION_HOOK(__imp__XAudioGetVoiceCategoryVolume, XAudioGetVoiceCategoryVolume);
