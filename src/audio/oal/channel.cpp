#include "common.h"

#include "../steamaudio.h"

#ifdef AUDIO_OAL
#include "channel.h"
#include "sampman.h"

#ifndef _WIN32
#include <float.h>
#endif

extern bool IsFXSupported();

ALuint alSources[NUM_CHANNELS];
ALuint alFilters[NUM_CHANNELS];
ALuint alBuffers[NUM_CHANNELS];
bool bChannelsCreated = false;

int32 CChannel::channelsThatNeedService = 0;

uint8 tempStereoBuffer[PED_BLOCKSIZE * 2];

void
CChannel::InitChannels()
{
	alGenSources(NUM_CHANNELS, alSources);
	alGenBuffers(NUM_CHANNELS, alBuffers);
	if (IsFXSupported())
		alGenFilters(NUM_CHANNELS, alFilters);
	bChannelsCreated = true;
}

void
CChannel::DestroyChannels()
{
	if (bChannelsCreated) 
	{
		alDeleteSources(NUM_CHANNELS, alSources);
		memset(alSources, 0, sizeof(alSources));
		alDeleteBuffers(NUM_CHANNELS, alBuffers);
		memset(alBuffers, 0, sizeof(alBuffers));
		if (IsFXSupported())
		{
			alDeleteFilters(NUM_CHANNELS, alFilters);
			memset(alFilters, 0, sizeof(alFilters));
		}
		bChannelsCreated = false;
	}
}


CChannel::CChannel()
{
	Data = nil;
	DataSize = 0;
	bIs2D = false;
	SetDefault();
}

void CChannel::SetDefault()
{
	Pitch = 1.0f;
	Gain = 1.0f;
	Mix = 0.0f;
		
	Position[0] = 0.0f; Position[1] = 0.0f; Position[2] = 0.0f;
	Distances[0] = 0.0f; Distances[1] = FLT_MAX;

	LoopCount = 1;
	LastProcessedOffset = UINT32_MAX;
	LoopPoints[0] = 0; LoopPoints[1] = -1;
	
	Frequency = MAX_FREQ;
}

void CChannel::Reset()
{
	// Here is safe because ctor don't call this
	if (LoopCount > 1)
		channelsThatNeedService--;

	ClearBuffer();
	SetDefault();
}

void CChannel::Init(uint32 _id, bool Is2D)
{
	id = _id;
	if ( HasSource() )
	{
#ifdef USE_STEAMAUDIO
		if (!SA::usingSteamAudio) {
			SA::usingSteamAudio = SA::InitSteamAudio();
			SA::InitFX();
		}
		
		if (SA::sound_sources.size() >= SA::MAX_SOUND_SOURCE_NUM) {
			SA::sound_sources.clear();
		}
		SA::SoundSource sound_source{};
		SA::sound_sources.emplace(id, std::move(sound_source));
#endif
	
		alSourcei(alSources[id], AL_SOURCE_RELATIVE, AL_TRUE);
#ifdef USE_STEAMAUDIO
		// Steam Audio handles all 3D spatialization; keep OpenAL centered.
		alSource3f(alSources[id], AL_POSITION, 0.0f, 0.0f, 0.0f);
		alSourcef(alSources[id], AL_GAIN, 1.0f);
#else
		if ( IsFXSupported() )
			alSource3i(alSources[id], AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
#endif
		
		if ( Is2D )
		{
			bIs2D = true;
#ifndef USE_STEAMAUDIO
			alSource3f(alSources[id], AL_POSITION, 0.0f, 0.0f, 0.0f);
			alSourcef(alSources[id], AL_GAIN, 1.0f);
#endif
#ifdef USE_STEAMAUDIO
			SA::sound_sources[id].source_position = {0.0f, 0.0f, 0.0f};
#endif
		}
	}
}

void CChannel::Term()
{
	Stop();
	if ( HasSource() )
	{
#ifndef USE_STEAMAUDIO
		if ( IsFXSupported() )
		{
			alSource3i(alSources[id], AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
		}
#endif
	
#ifdef USE_STEAMAUDIO
		SA::sound_sources.erase(id);
#endif
	}
}

void CChannel::Start()
{
	if ( !HasSource() ) return;
	if ( !Data ) return;


#ifdef USE_STEAMAUDIO
	std::vector<int16_t> out_data{};
	{
		// ???? there is fix a bug, don't remove !!!!
		if (DataSize == 33492) {
			return;
		}
	
		const int16_t* src = static_cast<const int16_t*>(Data);
		size_t num_input_samples = DataSize / sizeof(int16_t);
		std::vector<float> in_data(num_input_samples);
		for (size_t i = 0; i < num_input_samples; ++i)
			in_data[i] = src[i] / 32768.0f;

		float upsample_ratio = 48000.0f / Frequency;
		size_t resampled_count = static_cast<size_t>(num_input_samples * upsample_ratio) + 1;
		std::vector<float> resampled_data(resampled_count);
		for (size_t i = 0; i < resampled_count; ++i)
		{
			float pos = i / upsample_ratio;
			size_t idx = static_cast<size_t>(pos);
			if (idx >= num_input_samples) idx = num_input_samples - 1;
			float frac = pos - idx;
			if (idx + 1 < num_input_samples)
				resampled_data[i] = in_data[idx] * (1.0f - frac) + in_data[idx + 1] * frac;
			else
				resampled_data[i] = in_data[idx];
		}

		float* output_stereo_buffer = static_cast<float*>(malloc(resampled_count * 2 * sizeof(float)));
		auto& sound_source_item = SA::sound_sources[id];
		sound_source_item.sample_rate = 48000;
		sound_source_item.channels = 1;
		sound_source_item.data = resampled_data.data();
		sound_source_item.ProcessSpatialAudio(output_stereo_buffer, resampled_count);

		float downsample_ratio = static_cast<float>(num_input_samples) / resampled_count;
		for (size_t i = 0; i < num_input_samples; ++i)
		{
			float pos = i / downsample_ratio;
			size_t idx = static_cast<size_t>(pos);
			float frac = pos - idx;
			float left, right;
			if (idx + 1 < resampled_count) {
				left  = output_stereo_buffer[idx * 2 + 0] * (1.0f - frac) + output_stereo_buffer[(idx + 1) * 2 + 0] * frac;
				right = output_stereo_buffer[idx * 2 + 1] * (1.0f - frac) + output_stereo_buffer[(idx + 1) * 2 + 1] * frac;
			} else {
				left  = output_stereo_buffer[idx * 2 + 0];
				right = output_stereo_buffer[idx * 2 + 1];
			}
			out_data.push_back(static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, left * 32767.0f))));
			out_data.push_back(static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, right * 32767.0f))));
		}
		free(output_stereo_buffer);
	}
#endif
	
	if ( bIs2D )
	{
#ifdef USE_STEAMAUDIO
		alBufferData(alBuffers[id], AL_FORMAT_STEREO16, out_data.data(), out_data.size() * sizeof(int16_t), Frequency);
#else
		// convert mono data to stereo
		int16 *monoData = (int16*)Data;
		int16 *stereoData = (int16*)tempStereoBuffer;
		for (size_t i = 0; i < DataSize / 2; i++)
		{
			*(stereoData++) = *monoData;
			*(stereoData++) = *(monoData++);
		}
		alBufferData(alBuffers[id], AL_FORMAT_STEREO16, tempStereoBuffer, DataSize * 2, Frequency);
#endif
	}
	else
		alBufferData(alBuffers[id], AL_FORMAT_STEREO16, out_data.data(), out_data.size() * sizeof(int16_t), Frequency);
	if ( LoopPoints[0] != 0 && LoopPoints[0] != -1 )
		alBufferiv(alBuffers[id], AL_LOOP_POINTS_SOFT, LoopPoints);
	alSourcei(alSources[id], AL_BUFFER, alBuffers[id]);
	alSourcePlay(alSources[id]);
}

void CChannel::Stop()
{
	if ( HasSource() )
		alSourceStop(alSources[id]);
	
	Reset();
}

bool CChannel::HasSource()
{
	return alSources[id] != AL_NONE;
}
	
bool CChannel::IsUsed()
{
	if ( HasSource() )
	{
		ALint sourceState;
		alGetSourcei(alSources[id], AL_SOURCE_STATE, &sourceState);
		return sourceState == AL_PLAYING;
	}
	return false;
}

void CChannel::SetPitch(float pitch)
{
	if ( !HasSource() ) return;
	alSourcef(alSources[id], AL_PITCH, pitch);
}

void CChannel::SetGain(float gain)
{
	if ( !HasSource() ) return;
#ifdef USE_STEAMAUDIO
	SA::sound_sources[id].gain = gain;
#endif
	alSourcef(alSources[id], AL_GAIN, gain);
}
	
void CChannel::SetVolume(int32 vol)
{
	SetGain(ALfloat(vol) / MAX_VOLUME);
}

void CChannel::SetSampleData(void *_data, size_t _DataSize, int32 freq)
{
	Data = _data;
	DataSize = _DataSize;
	Frequency = freq;
}
	
void CChannel::SetCurrentFreq(uint32 freq)
{
	SetPitch(ALfloat(freq) / Frequency);
}

void CChannel::SetLoopCount(int32 count)
{
	if ( !HasSource() ) return;

	// 0: loop indefinitely, 1: play one time, 2: play two times etc...
	// only > 1 needs manual processing

	if (LoopCount > 1 && count < 2)
		channelsThatNeedService--;
	else if (LoopCount < 2 && count > 1)
		channelsThatNeedService++;

	alSourcei(alSources[id], AL_LOOPING, count == 1 ? AL_FALSE : AL_TRUE);
	LoopCount = count;
}

bool CChannel::Update()
{
	if (!HasSource()) return false;
	if (LoopCount < 2) return false;

	ALint state;
	alGetSourcei(alSources[id], AL_SOURCE_STATE, &state);
	if (state == AL_STOPPED) {
		debug("Looping channels(%d in this case) shouldn't report AL_STOPPED, but nvm\n", id);
		SetLoopCount(1);
		return true;
	}

	assert(channelsThatNeedService > 0 && "Ref counting is broken");

	ALint offset;
	alGetSourcei(alSources[id], AL_SAMPLE_OFFSET, &offset);

	// Rewound
	if (offset < LastProcessedOffset) {
		LoopCount--;
		if (LoopCount == 1) {
			// Playing last tune...
			channelsThatNeedService--;
			alSourcei(alSources[id], AL_LOOPING, AL_FALSE);
		}
	}
	LastProcessedOffset = offset;
	return true;
}

void CChannel::SetLoopPoints(ALint start, ALint end)
{
	LoopPoints[0] = start;
	LoopPoints[1] = end;
}
	
void CChannel::SetPosition(float x, float y, float z)
{
	if ( !HasSource() ) return;
#ifdef USE_STEAMAUDIO
	SA::sound_sources[id].source_position = {
		x,
		y,
		z
	};
#else
	alSource3f(alSources[id], AL_POSITION, x, y, z);
#endif
}
	
void CChannel::SetDistances(float max, float min)
{
	if ( !HasSource() ) return;
#ifdef USE_STEAMAUDIO
	SA::sound_sources[id].dist_max = max;
	SA::sound_sources[id].dist_min = min;
#else
	alSourcef   (alSources[id], AL_MAX_DISTANCE,       max);
	alSourcef   (alSources[id], AL_REFERENCE_DISTANCE, min);
	alSourcef   (alSources[id], AL_MAX_GAIN, 1.0f);
	alSourcef   (alSources[id], AL_ROLLOFF_FACTOR, 1.0f);
#endif
}
	
void CChannel::SetPan(int32 pan)
{
	SetPosition((pan-63)/64.0f, 0.0f, Sqrt(1.0f-SQR((pan-63)/64.0f)));
}

void CChannel::ClearBuffer()
{
	if ( !HasSource() ) return;
	alSourcei(alSources[id], AL_LOOPING, AL_FALSE);
	alSourcei(alSources[id], AL_BUFFER, AL_NONE);
	Data = nil;
	DataSize = 0;
}

void CChannel::SetReverbMix(ALuint slot, float mix)
{
#ifdef USE_STEAMAUDIO
	return;
#endif
	if ( !IsFXSupported() ) return;
	if ( !HasSource() ) return;
	if ( alFilters[id] == AL_FILTER_NULL ) return;
	
	Mix = mix;
	EAX3_SetReverbMix(alFilters[id], mix);
	alSource3i(alSources[id], AL_AUXILIARY_SEND_FILTER, slot, 0, alFilters[id]);
}

void CChannel::UpdateReverb(ALuint slot)
{
#ifdef USE_STEAMAUDIO
	return;
#endif
	if ( !IsFXSupported() ) return;
	if ( !HasSource() ) return;
	if ( alFilters[id] == AL_FILTER_NULL ) return;
	EAX3_SetReverbMix(alFilters[id], Mix);
	alSource3i(alSources[id], AL_AUXILIARY_SEND_FILTER, slot, 0, alFilters[id]);
}

#endif
