#include <stdafx.h>
#include "hid.h"

#include <array>
#include <mutex>

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

bool hid::IsInputAllowed()
{
	return true;
}

bool hid::IsInputDeviceController()
{
	return true;
}
