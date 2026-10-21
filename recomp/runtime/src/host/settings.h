// Ips3: player settings -- changed in the launcher menu (the "menu" app), kept in settings.txt
// next to the log, and applied when the game starts.
#pragma once

#include <filesystem>

namespace host
{
	struct Settings
	{
		bool showFps = false;
		int touchControls = 0;      // 0 automatic (no controller), 1 always, 2 never
		int touchOpacity = 100;     // percent
		bool sound = true;
		bool fastCores = true;      // ask iOS to keep the game and GPU threads on the fast cores
		bool asyncGpuSubmit = true; // MoltenVK hands work to the GPU without waiting
		int resolutionScale = 1;    // the GPU renders at 1x or 2x the console's resolution
		bool statusLog = true;      // the [status] lines in the log
	};

	// The settings in effect (loaded once, the first time).
	Settings& CurrentSettings();
	void SaveSettings();

	// Puts the settings that must be known before the GPU starts into effect (environment for
	// MoltenVK and Xenia, Xenia's resolution scale).
	void ApplyStartupSettings();

	// The launcher menu runs before the game: in the menu app (iOS: Ips3Menu in Info.plist), or
	// with IPS3_MENU set.
	bool MenuEnabled();
}
