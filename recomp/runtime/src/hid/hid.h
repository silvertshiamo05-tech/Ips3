// Controller input for the guest's XamInput* calls.
//
// The host (the iOS app, or a test harness) pushes pad state in with hid::SetHostState; the
// kernel's XamInputGetState reads it back here. Same interface UnleashedRecomp's SDL-based hid
// module exposes, so the adapted xam.cpp needs no further changes.
#pragma once

#include <cstdint>
#include <xbox.h>

namespace hid
{
	// Set by the game's own UI code paths that temporarily block buttons (kept for parity).
	inline uint16_t g_prohibitedButtons = 0;
	inline bool g_isLeftStickProhibited = false;
	inline bool g_isRightStickProhibited = false;

	struct HostPadState
	{
		bool connected = false;
		uint16_t buttons = 0; // XAMINPUT_GAMEPAD_*
		uint8_t leftTrigger = 0;
		uint8_t rightTrigger = 0;
		int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
	};

	// Called by the host for player 0..3.
	void SetHostState(uint32_t userIndex, const HostPadState& state);
	// Last vibration the game asked for, for the host to forward to the controller.
	void GetHostVibration(uint32_t userIndex, uint16_t& left, uint16_t& right);

	uint32_t GetState(uint32_t dwUserIndex, XAMINPUT_STATE* pState);
	uint32_t SetState(uint32_t dwUserIndex, XAMINPUT_VIBRATION* pVibration);
	uint32_t GetCapabilities(uint32_t dwUserIndex, XAMINPUT_CAPABILITIES* pCaps);

	bool IsInputAllowed();
	bool IsInputDeviceController();
}
