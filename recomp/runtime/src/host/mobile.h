// Ips3: host helpers for the phone and tablet builds (iOS: host/ios_files.mm, Android:
// host/android_files.cpp) -- where files go, and the disc image picker.
#pragma once

#include <SDL.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__ANDROID__)
#define IPS3_MOBILE 1
#else
#define IPS3_MOBILE 0
#endif

#include <filesystem>
#include <functional>

namespace mobile
{
	// The app's own folder for saves and the log: Documents on iOS (shown in the Files app),
	// Android/data/<app>/files on Android.
	std::filesystem::path DataPath();

	// Sends stdout and stderr to bayonetta-log.txt in DataPath().
	void RedirectLog();

	// The disc image picked on an earlier launch, else the first .iso in DataPath(), else empty.
	std::filesystem::path FindGameImage();

	// Forgets the remembered disc image (it did not start), so the picker shows again.
	void ForgetGameImage();

	// What the app says before showing the picker.
	const char* PickPrompt();

	// Shows the system file picker for a disc image. `picked` runs on the main thread, from
	// PollPicker on Android, with the chosen file (read in place; remembered for later
	// launches), or an empty path if cancelled.
	void PickGameImage(SDL_Window* window, std::function<void(const std::filesystem::path&)> picked);

	// The launcher-menu build of the app (iOS: Ips3Menu in Info.plist).
	bool MenuApp();

	// Called every frame: delivers a finished pick (Android's picker answers asynchronously).
	void PollPicker();
}
