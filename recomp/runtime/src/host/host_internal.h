// Ips3: pieces of the SDL host layer shared by host_sdl.cpp (audio, controllers, the headless
// input thread) and host_window.cpp (the window, touch controls and frame display).
#pragma once

#include <SDL.h>

#include <cstdint>

namespace host
{
	// Stick deflection from the touch controls, -1..1 with +Y up; 0 leaves the controller's.
	struct TouchSticks
	{
		float leftX = 0, leftY = 0, rightX = 0, rightY = 0;
	};

	// Opens and closes game controllers as they come and go.
	void ProcessControllerEvent(const SDL_Event& event);
	bool ControllerConnected();

	// Publishes player 0's pad: the first controller, plus the touch controls' buttons
	// (XAMINPUT_GAMEPAD_* mask), triggers (bit 0 left, bit 1 right) and sticks, plus scripted
	// presses. Forwards the game's rumble to the controller.
	void UpdatePad(uint16_t extraButtons, uint8_t extraTriggers, const TouchSticks& sticks);
}
