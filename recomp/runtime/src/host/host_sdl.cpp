// Ips3: SDL2 host layer. Audio goes out through an SDL queue; game controllers are polled on a
// host thread and handed to the guest's XamInput* calls through hid::SetHostState.
//
// Environment:
//   IPS3_AUDIO_WAV=<file>   also write everything played to a 48 kHz stereo float WAV file.
//   IPS3_INPUT=<script>     scripted pad input for headless runs: "ms:BUTTON:hold_ms;..." with
//                           times from start-up, e.g. "30000:START:150;32000:A:150".
#include <stdafx.h>
#include "host.h"

#include <hid/hid.h>

#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	SDL_AudioDeviceID g_audioDevice = 0;
	FILE* g_wav = nullptr;
	uint32_t g_wavFrames = 0;
	std::mutex g_wavMutex;

	constexpr int SAMPLE_RATE = 48000;
	constexpr int CHANNELS = 2;

	void WriteWavHeader()
	{
		const uint32_t dataBytes = g_wavFrames * CHANNELS * sizeof(float);
		auto u32 = [](uint32_t v) { fwrite(&v, 4, 1, g_wav); };
		auto u16 = [](uint16_t v) { fwrite(&v, 2, 1, g_wav); };
		fseek(g_wav, 0, SEEK_SET);
		fwrite("RIFF", 1, 4, g_wav); u32(36 + dataBytes); fwrite("WAVEfmt ", 1, 8, g_wav);
		u32(16); u16(3 /* IEEE float */); u16(CHANNELS); u32(SAMPLE_RATE);
		u32(SAMPLE_RATE * CHANNELS * sizeof(float)); u16(CHANNELS * sizeof(float)); u16(32);
		fwrite("data", 1, 4, g_wav); u32(dataBytes);
		fseek(g_wav, 0, SEEK_END);
	}

	struct ScriptedPress
	{
		uint32_t at, hold;
		uint16_t button;
		bool logged = false;
	};

	uint16_t ButtonByName(const std::string& name)
	{
		static const std::pair<const char*, uint16_t> names[] =
		{
			{ "UP", XAMINPUT_GAMEPAD_DPAD_UP }, { "DOWN", XAMINPUT_GAMEPAD_DPAD_DOWN },
			{ "LEFT", XAMINPUT_GAMEPAD_DPAD_LEFT }, { "RIGHT", XAMINPUT_GAMEPAD_DPAD_RIGHT },
			{ "START", XAMINPUT_GAMEPAD_START }, { "BACK", XAMINPUT_GAMEPAD_BACK },
			{ "LS", XAMINPUT_GAMEPAD_LEFT_THUMB }, { "RS", XAMINPUT_GAMEPAD_RIGHT_THUMB },
			{ "LB", XAMINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
			{ "A", XAMINPUT_GAMEPAD_A }, { "B", XAMINPUT_GAMEPAD_B },
			{ "X", XAMINPUT_GAMEPAD_X }, { "Y", XAMINPUT_GAMEPAD_Y },
		};
		for (const auto& [n, b] : names)
			if (name == n)
				return b;
		fmt::println(stderr, "[host] unknown button in IPS3_INPUT: {}", name);
		return 0;
	}

	std::vector<ScriptedPress> ParseScript(const char* text)
	{
		std::vector<ScriptedPress> presses;
		std::string s = text;
		size_t pos = 0;
		while (pos < s.size())
		{
			size_t end = s.find(';', pos);
			std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
			pos = end == std::string::npos ? s.size() : end + 1;

			const size_t a = item.find(':'), b = item.find(':', a + 1);
			if (a == std::string::npos || b == std::string::npos)
				continue;
			presses.push_back({ uint32_t(std::stoul(item.substr(0, a))), uint32_t(std::stoul(item.substr(b + 1))),
				ButtonByName(item.substr(a + 1, b - a - 1)), false });
		}
		return presses;
	}

	int16_t Axis(SDL_GameController* pad, SDL_GameControllerAxis axis, bool invert)
	{
		const int v = SDL_GameControllerGetAxis(pad, axis);
		return int16_t(std::clamp(invert ? -v - 1 : v, -32768, 32767));
	}

	void ReadController(SDL_GameController* pad, hid::HostPadState& state)
	{
		static const std::pair<SDL_GameControllerButton, uint16_t> buttons[] =
		{
			{ SDL_CONTROLLER_BUTTON_DPAD_UP, XAMINPUT_GAMEPAD_DPAD_UP },
			{ SDL_CONTROLLER_BUTTON_DPAD_DOWN, XAMINPUT_GAMEPAD_DPAD_DOWN },
			{ SDL_CONTROLLER_BUTTON_DPAD_LEFT, XAMINPUT_GAMEPAD_DPAD_LEFT },
			{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XAMINPUT_GAMEPAD_DPAD_RIGHT },
			{ SDL_CONTROLLER_BUTTON_START, XAMINPUT_GAMEPAD_START },
			{ SDL_CONTROLLER_BUTTON_BACK, XAMINPUT_GAMEPAD_BACK },
			{ SDL_CONTROLLER_BUTTON_LEFTSTICK, XAMINPUT_GAMEPAD_LEFT_THUMB },
			{ SDL_CONTROLLER_BUTTON_RIGHTSTICK, XAMINPUT_GAMEPAD_RIGHT_THUMB },
			{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XAMINPUT_GAMEPAD_LEFT_SHOULDER },
			{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
			{ SDL_CONTROLLER_BUTTON_A, XAMINPUT_GAMEPAD_A },
			{ SDL_CONTROLLER_BUTTON_B, XAMINPUT_GAMEPAD_B },
			{ SDL_CONTROLLER_BUTTON_X, XAMINPUT_GAMEPAD_X },
			{ SDL_CONTROLLER_BUTTON_Y, XAMINPUT_GAMEPAD_Y },
		};
		for (const auto& [sdl, xam] : buttons)
			if (SDL_GameControllerGetButton(pad, sdl))
				state.buttons |= xam;

		state.leftTrigger = uint8_t(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
		state.rightTrigger = uint8_t(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
		// SDL's Y axes point down, XInput's up.
		state.thumbLX = Axis(pad, SDL_CONTROLLER_AXIS_LEFTX, false);
		state.thumbLY = Axis(pad, SDL_CONTROLLER_AXIS_LEFTY, true);
		state.thumbRX = Axis(pad, SDL_CONTROLLER_AXIS_RIGHTX, false);
		state.thumbRY = Axis(pad, SDL_CONTROLLER_AXIS_RIGHTY, true);
	}

	// Polls controllers about every 4 ms (the console's input rate) and forwards rumble.
	void InputThread(std::vector<ScriptedPress> script)
	{
		std::vector<SDL_GameController*> pads;
		const auto start = std::chrono::steady_clock::now();

		for (;;)
		{
			SDL_Event event;
			while (SDL_PollEvent(&event))
			{
				if (event.type == SDL_CONTROLLERDEVICEADDED)
				{
					if (SDL_GameController* pad = SDL_GameControllerOpen(event.cdevice.which))
					{
						pads.push_back(pad);
						fmt::println(stderr, "[host] controller connected: {}", SDL_GameControllerName(pad));
					}
				}
				else if (event.type == SDL_CONTROLLERDEVICEREMOVED)
				{
					std::erase_if(pads, [&](SDL_GameController* pad)
					{
						if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) != event.cdevice.which)
							return false;
						SDL_GameControllerClose(pad);
						return true;
					});
				}
			}

			// Player 0 is always present: the first controller plus any scripted presses. The game
			// treats a missing pad as unplugged and pauses on it.
			hid::HostPadState state;
			state.connected = true;
			if (!pads.empty())
				ReadController(pads.front(), state);

			const uint32_t now = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count());
			for (auto& press : script)
			{
				if (now >= press.at && now < press.at + press.hold)
				{
					state.buttons |= press.button;
					if (!press.logged)
					{
						press.logged = true;
						fmt::println(stderr, "[host] scripted press {:04X} at {} ms", press.button, now);
					}
				}
			}

			hid::SetHostState(0, state);

			if (!pads.empty())
			{
				uint16_t left, right;
				hid::GetHostVibration(0, left, right);
				SDL_GameControllerRumble(pads.front(), left, right, 100);
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	}
}

void host::Init()
{
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	if (SDL_Init(SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
		fmt::println(stderr, "[host] SDL controllers unavailable: {}", SDL_GetError());

	if (const char* path = getenv("IPS3_AUDIO_WAV"))
	{
		g_wav = fopen(path, "wb");
		if (g_wav)
			WriteWavHeader();
	}

	std::vector<ScriptedPress> script;
	if (const char* text = getenv("IPS3_INPUT"))
		script = ParseScript(text);

	// Player 0 reads as connected from the first frame on.
	hid::SetHostState(0, hid::HostPadState{ .connected = true });
	std::thread(InputThread, std::move(script)).detach();
}

bool host::AudioOpen()
{
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
	{
		fmt::println(stderr, "[host] no audio: {}", SDL_GetError());
		return false;
	}

	SDL_AudioSpec want{}, have{};
	want.freq = SAMPLE_RATE;
	want.format = AUDIO_F32SYS;
	want.channels = CHANNELS;
	want.samples = 512;
	g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
	if (g_audioDevice == 0)
	{
		fmt::println(stderr, "[host] no audio device: {}", SDL_GetError());
		return false;
	}

	fmt::println(stderr, "[host] audio: {} at {} Hz", SDL_GetCurrentAudioDriver(), have.freq);
	SDL_PauseAudioDevice(g_audioDevice, 0);
	return true;
}

void host::AudioQueue(const float* samples, size_t frames)
{
	if (g_audioDevice != 0)
		SDL_QueueAudio(g_audioDevice, samples, uint32_t(frames * CHANNELS * sizeof(float)));

	if (g_wav)
	{
		std::lock_guard lock(g_wavMutex);
		fwrite(samples, sizeof(float) * CHANNELS, frames, g_wav);
		g_wavFrames += uint32_t(frames);
		if (g_wavFrames % (SAMPLE_RATE / 2) < frames) // keep the header valid every half second
		{
			WriteWavHeader();
			fflush(g_wav);
		}
	}
}

size_t host::AudioQueuedFrames()
{
	if (g_audioDevice == 0)
		return 0;
	return SDL_GetQueuedAudioSize(g_audioDevice) / (CHANNELS * sizeof(float));
}
