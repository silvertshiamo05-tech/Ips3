#include "stdafx.h"
#include "CoreAudioBackend.h"

#include "Emu/system_config.h"

#include <algorithm>
#include <chrono>
#include <cstring>

LOG_CHANNEL(CoreAudio, "CoreAudio");

CoreAudioBackend::CoreAudioBackend()
	: AudioBackend()
{
}

CoreAudioBackend::~CoreAudioBackend()
{
	Close();
}

bool CoreAudioBackend::Operational()
{
	return m_unit && !m_reset_req.observe();
}

bool CoreAudioBackend::Open(std::string_view /*dev_id*/, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout)
{
	Close();

	AudioComponentDescription desc{};
	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = kAudioUnitSubType_RemoteIO;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;

	AudioComponent component = AudioComponentFindNext(nullptr, &desc);

	if (!component || AudioComponentInstanceNew(component, &m_unit) != noErr)
	{
		CoreAudio.error("RemoteIO audio unit is not available");
		m_unit = nullptr;
		return false;
	}

	// Stereo out: the AVAudioSession route is a speaker pair or headphones in practice, and
	// AudioBackend downmixes anything wider (setup_channel_layout below).
	const u32 out_channels = std::min<u32>(static_cast<u32>(ch_cnt), 2);
	const bool is_float = sample_size == AudioSampleSize::FLOAT;
	const u32 bytes_per_sample = is_float ? sizeof(f32) : sizeof(s16);

	AudioStreamBasicDescription fmt{};
	fmt.mSampleRate = static_cast<f64>(static_cast<u32>(freq));
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mFormatFlags = (is_float ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) | kAudioFormatFlagIsPacked;
	fmt.mChannelsPerFrame = out_channels;
	fmt.mBitsPerChannel = bytes_per_sample * 8;
	fmt.mFramesPerPacket = 1;
	fmt.mBytesPerFrame = bytes_per_sample * out_channels;
	fmt.mBytesPerPacket = fmt.mBytesPerFrame;

	OSStatus err = AudioUnitSetProperty(m_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));

	if (err == noErr)
	{
		AURenderCallbackStruct cb{};
		cb.inputProc = &CoreAudioBackend::render;
		cb.inputProcRefCon = this;
		err = AudioUnitSetProperty(m_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
	}

	if (err == noErr)
	{
		err = AudioUnitInitialize(m_unit);
	}

	if (err != noErr)
	{
		CoreAudio.error("Failed to set up RemoteIO (OSStatus %d)", static_cast<s32>(err));
		AudioComponentInstanceDispose(m_unit);
		m_unit = nullptr;
		return false;
	}

	m_sampling_rate = freq;
	m_sample_size = sample_size;
	m_channels = out_channels;

	setup_channel_layout(static_cast<u32>(ch_cnt), m_channels, layout, CoreAudio);

	m_full_sample_size = static_cast<u8>(m_channels * get_sample_size());
	m_reset_req = false;

	CoreAudio.notice("Opened RemoteIO: %uHz, %u channels, %s", static_cast<u32>(freq), m_channels, is_float ? "f32" : "s16");
	return true;
}

void CoreAudioBackend::Close()
{
	if (!m_unit)
	{
		return;
	}

	// Stopping joins the render thread, so nothing below races a callback.
	AudioOutputUnitStop(m_unit);
	AudioUnitUninitialize(m_unit);
	AudioComponentInstanceDispose(m_unit);
	m_unit = nullptr;

	std::lock_guard lock(m_cb_mutex);
	m_playing = false;
	m_last_sample.fill(0);
}

f64 CoreAudioBackend::GetCallbackFrameLen()
{
	// RemoteIO renders in IO buffers of the session's preferred duration (10 ms, set by the
	// app); report that so cellAudio sizes its periods sensibly.
	return 0.010;
}

void CoreAudioBackend::Play()
{
	if (!m_unit)
	{
		return;
	}

	{
		std::lock_guard lock(m_cb_mutex);

		if (m_playing)
		{
			return;
		}
	}

	if (const OSStatus err = AudioOutputUnitStart(m_unit); err != noErr)
	{
		CoreAudio.error("AudioOutputUnitStart failed (OSStatus %d)", static_cast<s32>(err));
		m_reset_req = true;
		return;
	}

	std::lock_guard lock(m_cb_mutex);
	m_playing = true;
}

void CoreAudioBackend::Pause()
{
	if (!m_unit || !m_playing)
	{
		return;
	}

	AudioOutputUnitStop(m_unit);

	std::lock_guard lock(m_cb_mutex);
	m_playing = false;
	m_last_sample.fill(0);
}

OSStatus CoreAudioBackend::render(void* ref, AudioUnitRenderActionFlags* /*flags*/, const AudioTimeStamp* /*ts*/, UInt32 /*bus*/, UInt32 frames, AudioBufferList* data)
{
	auto* self = static_cast<CoreAudioBackend*>(ref);

	if (!data || data->mNumberBuffers < 1 || !data->mBuffers[0].mData)
	{
		return noErr;
	}

	u8* const out = static_cast<u8*>(data->mBuffers[0].mData);
	const u32 sample_size = self->m_full_sample_size.observe();

	if (!sample_size)
	{
		std::memset(out, 0, data->mBuffers[0].mDataByteSize);
		return noErr;
	}

	const u32 bytes_req = std::min<u32>(frames * sample_size, data->mBuffers[0].mDataByteSize);

	// Real-time thread: never block on the emulator.
	std::unique_lock lock(self->m_cb_mutex, std::defer_lock);
	u32 written = 0;

	if (lock.try_lock_for(std::chrono::microseconds{50}) && self->m_write_callback && self->m_playing)
	{
		written = std::min(self->m_write_callback(bytes_req, out), bytes_req);
		written -= written % sample_size;

		if (written >= sample_size)
		{
			std::memcpy(self->m_last_sample.data(), out + written - sample_size, sample_size);
		}
	}

	if (written < bytes_req)
	{
		self->m_underruns++;
	}

	// Hold the last sample rather than leaving a gap.
	for (u32 i = written; i + sample_size <= bytes_req; i += sample_size)
	{
		std::memcpy(out + i, self->m_last_sample.data(), sample_size);
	}

	return noErr;
}
