// Ips3: player settings, as "key = value" lines in settings.txt.
#include "settings.h"
#include "mobile.h"

#include <ips3_gpu.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

namespace
{
	std::filesystem::path SettingsPath()
	{
#if IPS3_MOBILE
		return mobile::DataPath() / "settings.txt";
#else
		return "ips3-settings.txt";
#endif
	}

	host::Settings Load()
	{
		host::Settings settings;
		std::ifstream file(SettingsPath());
		for (std::string line; std::getline(file, line);)
		{
			const size_t equals = line.find('=');
			if (equals == std::string::npos)
				continue;
			auto trim = [](std::string text)
			{
				const size_t begin = text.find_first_not_of(" \t\r");
				const size_t end = text.find_last_not_of(" \t\r");
				return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
			};
			const std::string key = trim(line.substr(0, equals));
			const int value = atoi(trim(line.substr(equals + 1)).c_str());
			if (key == "show_fps") settings.showFps = value != 0;
			else if (key == "touch_controls") settings.touchControls = std::clamp(value, 0, 2);
			else if (key == "touch_opacity") settings.touchOpacity = std::clamp(value, 25, 100);
			else if (key == "sound") settings.sound = value != 0;
			else if (key == "fast_cores") settings.fastCores = value != 0;
			else if (key == "async_gpu_submit") settings.asyncGpuSubmit = value != 0;
			else if (key == "resolution_scale") settings.resolutionScale = std::clamp(value, 1, 2);
			else if (key == "status_log") settings.statusLog = value != 0;
			else if (key == "render_path") settings.renderPath = std::clamp(value, 0, 1);
			else if (key == "metal_argument_buffers") settings.metalArgumentBuffers = value != 0;
			else if (key == "small_render_areas") settings.smallRenderAreas = value != 0;
		}
		return settings;
	}
}

host::Settings& host::CurrentSettings()
{
	static Settings settings = Load();
	return settings;
}

void host::SaveSettings()
{
	const Settings& s = CurrentSettings();
	std::ofstream file(SettingsPath());
	file << "show_fps = " << s.showFps << "\n"
		<< "touch_controls = " << s.touchControls << "\n"
		<< "touch_opacity = " << s.touchOpacity << "\n"
		<< "sound = " << s.sound << "\n"
		<< "fast_cores = " << s.fastCores << "\n"
		<< "async_gpu_submit = " << s.asyncGpuSubmit << "\n"
		<< "resolution_scale = " << s.resolutionScale << "\n"
		<< "status_log = " << s.statusLog << "\n"
		<< "render_path = " << s.renderPath << "\n"
		<< "metal_argument_buffers = " << s.metalArgumentBuffers << "\n"
		<< "small_render_areas = " << s.smallRenderAreas << "\n";
}

void host::ApplyStartupSettings()
{
	const Settings& s = CurrentSettings();
	// Read by MoltenVK when the Vulkan instance is created, and by the GPU command thread.
	setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", s.asyncGpuSubmit ? "0" : "1", 1);
	setenv("IPS3_FAST_CORES", s.fastCores ? "1" : "0", 1);
	ips3_gpu_set_resolution_scale(uint32_t(s.resolutionScale));
	if (!s.smallRenderAreas)
		setenv("IPS3_FULL_RENDER_AREA", "1", 1);
	if (!s.metalArgumentBuffers)
		setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 1);
	if (s.renderPath == 1)
	{
		// Xenia settings go through IPS3_XENIA (read when the GPU starts).
		std::string xenia = getenv("IPS3_XENIA") ? getenv("IPS3_XENIA") : "";
		xenia += std::string(xenia.empty() ? "" : ";") + "render_target_path_vulkan = \"fsi\"";
		setenv("IPS3_XENIA", xenia.c_str(), 1);
	}
	fmt::println(stderr, "[settings] resolution {}x, render path {}, Metal argument buffers {}, async GPU submit {}, "
		"fast cores {}, sound {}, small render areas {}", s.resolutionScale, s.renderPath == 1 ? "interlock" : "framebuffers",
		s.metalArgumentBuffers ? "on" : "off", s.asyncGpuSubmit ? "on" : "off", s.fastCores ? "on" : "off",
		s.sound ? "on" : "off", s.smallRenderAreas ? "on" : "off");
}

bool host::MenuEnabled()
{
	if (getenv("IPS3_MENU"))
		return true;
#if IPS3_MOBILE
	return mobile::MenuApp();
#else
	return false;
#endif
}
