#pragma once

#include "util/atomic.hpp"
#include "Emu/Audio/AudioBackend.h"

#include <AudioToolbox/AudioToolbox.h>

#include <array>

// iOS audio through a RemoteIO AudioUnit.
//
// cubeb's AudioUnit backend is written against the macOS HAL (AudioObject device
// enumeration, CoreAudio/AudioHardware.h) and does not build for iOS. RemoteIO is the
// lowest-latency output iOS offers and needs nothing else: the AVAudioSession the app
// configures decides the route.
class CoreAudioBackend final : public AudioBackend
{
public:
	CoreAudioBackend();
	~CoreAudioBackend() override;

	CoreAudioBackend(const CoreAudioBackend&) = delete;
	CoreAudioBackend& operator=(const CoreAudioBackend&) = delete;

	std::string_view GetName() const override { return "CoreAudio"sv; }

	bool Initialized() override { return true; }
	bool Operational() override;

	bool Open(std::string_view dev_id, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout) override;
	void Close() override;

	f64 GetCallbackFrameLen() override;

	void Play() override;
	void Pause() override;

private:
	static OSStatus render(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus, UInt32 frames, AudioBufferList* data);

	AudioComponentInstance m_unit = nullptr;

	std::array<u8, sizeof(f32) * static_cast<u32>(AudioChannelCnt::SURROUND_7_1)> m_last_sample{};
	atomic_t<u8> m_full_sample_size = 0;
	atomic_t<bool> m_reset_req = false;
};
