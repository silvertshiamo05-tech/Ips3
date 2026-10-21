// Ips3: the host side of the runtime -- sound out and controllers in -- on SDL2 (Linux, macOS,
// iOS). The kernel layer talks only to this interface and to hid::SetHostState.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace host
{
	// Starts SDL's controller subsystem; without a window also the thread that polls controllers.
	void Init();

	// True when the game shows in a window: always on iOS, on desktops when IPS3_WINDOW is set.
	bool WindowMode();

	// Runs the window on the calling (main) thread until it is closed, showing the game's frames
	// and the touch controls and feeding the pad. `start` is called once, on this thread, with the
	// disc image -- `image` if given, else (iOS) one from the Documents folder or the file
	// picker -- and starts the game on its own thread; it returns false if the game cannot start.
	int RunWindow(const std::filesystem::path& image, const std::function<bool(const std::filesystem::path&)>& start);

	// Reports a fatal problem: a message box with a window, stderr always.
	void ShowError(const std::string& message);

	// Opens a 48 kHz stereo float output. False when the host has no usable audio device; the
	// caller then paces the guest's audio by the clock instead.
	bool AudioOpen();
	// Queues interleaved stereo frames.
	void AudioQueue(const float* samples, size_t frames);
	// Frames queued and not yet played.
	size_t AudioQueuedFrames();
}
