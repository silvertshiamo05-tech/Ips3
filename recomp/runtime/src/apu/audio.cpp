// XAudio render driver for the guest.
//
// The game registers one callback; the system calls it every 256 samples (48 kHz, ~5.33 ms)
// with a pointer to a word holding the registered argument (as Xenia's AudioSystem does),
// and the game mixes a frame and hands it back through XAudioSubmitRenderDriverFrame (6 planar
// channels of 256 big-endian floats). Frames are downmixed to stereo and played by the host.
// With a host device the driver thread keeps its queue topped up, so the device's clock paces
// the game; without one it runs on a timer at the console's rate.
#include <stdafx.h>

#include <cpu/guest_thread.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <host/host.h>

namespace
{
	constexpr uint32_t DRIVER_HANDLE = 0x41550000;
	constexpr size_t FRAME_SAMPLES = 256;
	constexpr auto FRAME_PERIOD = std::chrono::nanoseconds(1'000'000'000LL * FRAME_SAMPLES / 48000);
	// Host queue target: about 32 ms, enough to ride out a slow guest frame.
	constexpr size_t QUEUE_TARGET = FRAME_SAMPLES * 6;

	std::atomic<uint32_t> g_callback{ 0 };
	std::atomic<uint32_t> g_callbackArg{ 0 };
	std::atomic<bool> g_running{ false };
	std::atomic<uint64_t> g_framesSubmitted{ 0 };
	std::atomic<uint64_t> g_framesAudible{ 0 };
	bool g_hostAudio = false;

	void DriverThread()
	{
		GuestThreadContext context(0);
		auto next = std::chrono::steady_clock::now();

		while (g_running)
		{
			if (g_hostAudio)
			{
				if (host::AudioQueuedFrames() >= QUEUE_TARGET)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
			}
			else
			{
				next += FRAME_PERIOD;
				std::this_thread::sleep_until(next);
			}

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
				fmt::println(stderr, "[audio] {} callbacks, {} frames submitted, {} audible", calls, g_framesSubmitted.load(), g_framesAudible.load());
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

	if (getenv("IPS3_TRACE_AUDIO"))
		fmt::println(stderr, "[audio] render client {:08X} argument {:08X}", uint32_t(callback[0]), uint32_t(callback[1]));

	if (!g_running.exchange(true))
	{
		g_hostAudio = host::AudioOpen();
		std::thread(DriverThread).detach();
	}

	return 0;
}

static uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver)
{
	g_callback = 0;
	return 0;
}

static uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, be<float>* samples)
{
	g_framesSubmitted++;
	if (samples == nullptr)
		return 0;

	// 5.1 planar (FL, FR, C, LFE, RL, RR) down to interleaved stereo.
	constexpr float CENTER = 0.7071f, SURROUND = 0.7071f;
	float stereo[FRAME_SAMPLES * 2];
	bool audible = false;
	for (size_t i = 0; i < FRAME_SAMPLES; i++)
	{
		const float fl = samples[0 * FRAME_SAMPLES + i], fr = samples[1 * FRAME_SAMPLES + i];
		const float c = samples[2 * FRAME_SAMPLES + i];
		const float rl = samples[4 * FRAME_SAMPLES + i], rr = samples[5 * FRAME_SAMPLES + i];
		const float l = fl + CENTER * c + SURROUND * rl;
		const float r = fr + CENTER * c + SURROUND * rr;
		stereo[i * 2 + 0] = std::clamp(l, -1.0f, 1.0f);
		stereo[i * 2 + 1] = std::clamp(r, -1.0f, 1.0f);
		audible |= std::abs(l) > 1e-4f || std::abs(r) > 1e-4f;
	}

	if (audible)
		g_framesAudible++;

	host::AudioQueue(stereo, FRAME_SAMPLES);
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
