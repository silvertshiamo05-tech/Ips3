// Ips3: the host side of the runtime -- sound out and controllers in -- on SDL2 (Linux, macOS,
// iOS). The kernel layer talks only to this interface and to hid::SetHostState.
#pragma once

#include <cstddef>
#include <cstdint>

namespace host
{
	// Starts SDL's audio and controller subsystems and the thread that polls controllers.
	void Init();

	// Opens a 48 kHz stereo float output. False when the host has no usable audio device; the
	// caller then paces the guest's audio by the clock instead.
	bool AudioOpen();
	// Queues interleaved stereo frames.
	void AudioQueue(const float* samples, size_t frames);
	// Frames queued and not yet played.
	size_t AudioQueuedFrames();
}
