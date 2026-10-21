#include <stdafx.h>
#include "hid.h"

#include <array>
#include <mutex>
#include <chrono>

namespace
{
	std::mutex g_mutex;
	std::array<hid::HostPadState, 4> g_pads{};
	std::array<std::pair<uint16_t, uint16_t>, 4> g_vibration{};

	constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED_ = 1167;
}

void hid::SetHostState(uint32_t userIndex, const HostPadState& state)
{
	if (userIndex >= g_pads.size())
		return;

	std::lock_guard lock(g_mutex);
	g_pads[userIndex] = state;
}

void hid::GetHostVibration(uint32_t userIndex, uint16_t& left, uint16_t& right)
{
	std::lock_guard lock(g_mutex);
	left = userIndex < 4 ? g_vibration[userIndex].first : 0;
	right = userIndex < 4 ? g_vibration[userIndex].second : 0;
}

uint32_t hid::GetState(uint32_t dwUserIndex, XAMINPUT_STATE* pState)
{
	if (dwUserIndex >= g_pads.size())
		return ERROR_DEVICE_NOT_CONNECTED_;

	std::lock_guard lock(g_mutex);
	const HostPadState& pad = g_pads[dwUserIndex];

	if (!pad.connected)
		return ERROR_DEVICE_NOT_CONNECTED_;

	static uint32_t s_packet = 0;
	pState->dwPacketNumber = ++s_packet;
	pState->Gamepad.wButtons = pad.buttons;
	pState->Gamepad.bLeftTrigger = pad.leftTrigger;
	pState->Gamepad.bRightTrigger = pad.rightTrigger;
	pState->Gamepad.sThumbLX = pad.thumbLX;
	pState->Gamepad.sThumbLY = pad.thumbLY;
	pState->Gamepad.sThumbRX = pad.thumbRX;
	pState->Gamepad.sThumbRY = pad.thumbRY;
	return 0;
}

uint32_t hid::SetState(uint32_t dwUserIndex, XAMINPUT_VIBRATION* pVibration)
{
	if (dwUserIndex >= g_pads.size())
		return ERROR_DEVICE_NOT_CONNECTED_;

	std::lock_guard lock(g_mutex);
	g_vibration[dwUserIndex] = { pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed };
	return 0;
}

uint32_t hid::GetCapabilities(uint32_t dwUserIndex, XAMINPUT_CAPABILITIES* pCaps)
{
	if (dwUserIndex >= g_pads.size())
		return ERROR_DEVICE_NOT_CONNECTED_;

	{
		std::lock_guard lock(g_mutex);
		if (!g_pads[dwUserIndex].connected)
			return ERROR_DEVICE_NOT_CONNECTED_;
	}

	memset(pCaps, 0, sizeof(*pCaps));
	pCaps->Type = 0x01;    // XINPUT_DEVTYPE_GAMEPAD
	pCaps->SubType = 0x01; // XINPUT_DEVSUBTYPE_GAMEPAD
	pCaps->Gamepad.wButtons = 0xF3FF;
	pCaps->Gamepad.bLeftTrigger = 0xFF;
	pCaps->Gamepad.bRightTrigger = 0xFF;
	pCaps->Gamepad.sThumbLX = int16_t(0xFFC0);
	pCaps->Gamepad.sThumbLY = int16_t(0xFFC0);
	pCaps->Gamepad.sThumbRX = int16_t(0xFFC0);
	pCaps->Gamepad.sThumbRY = int16_t(0xFFC0);
	pCaps->Vibration.wLeftMotorSpeed = 0xFF;
	pCaps->Vibration.wRightMotorSpeed = 0xFF;
	return 0;
}

namespace
{
	// XAM's virtual keys for the pad: buttons, triggers, then the stick directions.
	struct KeySource
	{
		uint16_t virtualKey;
		uint16_t button;   // XAMINPUT_GAMEPAD_* mask, or 0
		int8_t trigger;    // 0 left, 1 right, -1 none
		int8_t stick;      // 0 left, 1 right, -1 none
		int8_t direction;  // 0 up, 1 down, 2 right, 3 left
	};

	constexpr KeySource KEYS[] =
	{
		{ 0x5800, XAMINPUT_GAMEPAD_A, -1, -1, 0 }, { 0x5801, XAMINPUT_GAMEPAD_B, -1, -1, 0 },
		{ 0x5802, XAMINPUT_GAMEPAD_X, -1, -1, 0 }, { 0x5803, XAMINPUT_GAMEPAD_Y, -1, -1, 0 },
		{ 0x5804, XAMINPUT_GAMEPAD_RIGHT_SHOULDER, -1, -1, 0 }, { 0x5805, XAMINPUT_GAMEPAD_LEFT_SHOULDER, -1, -1, 0 },
		{ 0x5806, 0, 0, -1, 0 }, { 0x5807, 0, 1, -1, 0 },
		{ 0x5810, XAMINPUT_GAMEPAD_DPAD_UP, -1, -1, 0 }, { 0x5811, XAMINPUT_GAMEPAD_DPAD_DOWN, -1, -1, 0 },
		{ 0x5812, XAMINPUT_GAMEPAD_DPAD_LEFT, -1, -1, 0 }, { 0x5813, XAMINPUT_GAMEPAD_DPAD_RIGHT, -1, -1, 0 },
		{ 0x5814, XAMINPUT_GAMEPAD_START, -1, -1, 0 }, { 0x5815, XAMINPUT_GAMEPAD_BACK, -1, -1, 0 },
		{ 0x5816, XAMINPUT_GAMEPAD_LEFT_THUMB, -1, -1, 0 }, { 0x5817, XAMINPUT_GAMEPAD_RIGHT_THUMB, -1, -1, 0 },
		{ 0x5820, 0, -1, 0, 0 }, { 0x5821, 0, -1, 0, 1 }, { 0x5822, 0, -1, 0, 2 }, { 0x5823, 0, -1, 0, 3 },
		{ 0x5830, 0, -1, 1, 0 }, { 0x5831, 0, -1, 1, 1 }, { 0x5832, 0, -1, 1, 2 }, { 0x5833, 0, -1, 1, 3 },
	};
	constexpr size_t KEY_COUNT = std::size(KEYS);

	constexpr uint8_t TRIGGER_THRESHOLD = 30;   // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
	constexpr int STICK_THRESHOLD = 16384;
	constexpr auto REPEAT_DELAY = std::chrono::milliseconds(400);
	constexpr auto REPEAT_INTERVAL = std::chrono::milliseconds(100);

	bool IsDown(const hid::HostPadState& pad, const KeySource& key)
	{
		if (key.button)
			return (pad.buttons & key.button) != 0;
		if (key.trigger >= 0)
			return (key.trigger == 0 ? pad.leftTrigger : pad.rightTrigger) > TRIGGER_THRESHOLD;

		const int x = key.stick == 0 ? pad.thumbLX : pad.thumbRX;
		const int y = key.stick == 0 ? pad.thumbLY : pad.thumbRY;
		switch (key.direction)
		{
		case 0: return y > STICK_THRESHOLD;
		case 1: return y < -STICK_THRESHOLD;
		case 2: return x > STICK_THRESHOLD;
		default: return x < -STICK_THRESHOLD;
		}
	}

	struct KeyState
	{
		bool down = false;
		std::chrono::steady_clock::time_point nextRepeat;
	};
	std::array<std::array<KeyState, KEY_COUNT>, 4> g_keys{};
}

bool hid::GetKeystroke(uint32_t userIndex, Keystroke& keystroke)
{
	std::lock_guard lock(g_mutex);
	const auto now = std::chrono::steady_clock::now();

	for (uint32_t user = 0; user < g_pads.size(); user++)
	{
		if ((userIndex != 0xFF && user != userIndex) || !g_pads[user].connected)
			continue;

		for (size_t i = 0; i < KEY_COUNT; i++)
		{
			KeyState& key = g_keys[user][i];
			const bool down = IsDown(g_pads[user], KEYS[i]);
			uint16_t flags = 0;

			if (down != key.down)
			{
				key.down = down;
				key.nextRepeat = now + REPEAT_DELAY;
				flags = down ? 1 : 2;
			}
			else if (down && now >= key.nextRepeat)
			{
				key.nextRepeat = now + REPEAT_INTERVAL;
				flags = 1 | 4;
			}

			if (flags)
			{
				keystroke = { KEYS[i].virtualKey, 0, flags, uint8_t(user), 0 };
				return true;
			}
		}
	}

	return false;
}

bool hid::IsInputAllowed()
{
	return true;
}

bool hid::IsInputDeviceController()
{
	return true;
}
