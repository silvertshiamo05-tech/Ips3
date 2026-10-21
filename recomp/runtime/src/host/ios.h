// Ips3: iOS-only host helpers (host/ios_files.mm).
#pragma once

#include <SDL.h>

#include <filesystem>
#include <functional>

namespace ios
{
	// The app's Documents folder (shown in the Files app).
	std::filesystem::path DocumentsPath();

	// Sends stdout and stderr to Documents/bayonetta-log.txt.
	void RedirectLog();

	// The disc image picked on an earlier launch, else the first .iso in Documents, else empty.
	std::filesystem::path FindGameImage();

	// Shows the system file picker for a disc image. `picked` runs on the main thread with the
	// chosen file (read in place; remembered for later launches), or an empty path if cancelled.
	void PickGameImage(SDL_Window* window, std::function<void(const std::filesystem::path&)> picked);
}
