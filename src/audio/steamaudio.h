#pragma once

#include <cstdint>
#include <map>
#include <vector>

#ifdef USE_STEAMAUDIO
#include <phonon.h>
#include <cfloat>

namespace SA
{
    const uint32_t STEAM_AUDIO_FRAME_SIZE = 512;
    bool usingSteamAudio = false;
    constexpr int MAX_SOUND_SOURCE_NUM = 64;
    IPLContext context = nullptr;
    IPLAudioSettings audio_settings{};
    IPLHRTF hrtf = nullptr;
    IPLBinauralEffect bin_effect = nullptr;

    class SoundSource
    {
    public:
        IPLVector3 source_position = {0.0f, 0.0f, 0.0f};
        IPLVector3 listener_position = {0.0f, 0.0f, 0.0f};
        IPLVector3 listener_ahead = {0.0f, 0.0f, -1.0f};
        IPLVector3 listener_up = {0.0f, 1.0f, 0.0f};
        IPLAudioBuffer out_buffer{};
        float gain = 1.0f;
        float dist_min = 1.0f;
        float dist_max = FLT_MAX;

        std::vector<float> mono_input_buffer{};
        float* data = nullptr;
        uint64_t total_frames = 0;
        uint32_t channels = 0;
        uint32_t sample_rate = 0;
        bool is_playing_finished = false;
        size_t sample_frame_cursor = 0;

        ~SoundSource()
        {
            if (data)
            {
                data = nullptr;
            }
        }
        
        void ProcessSpatialAudio(float* output_stereo_buffer, size_t data_count)
        {
            if (!bin_effect || !data)
            {
                return;
            }

            const int32_t frame_size = audio_settings.frameSize;

            iplAudioBufferFree(context, &out_buffer);
            iplAudioBufferAllocate(context, 2, frame_size, &out_buffer);

            mono_input_buffer.resize(frame_size);
            
            IPLVector3 direction = iplCalculateRelativeDirection(
                context, source_position, listener_position, listener_ahead, listener_up);

            IPLBinauralEffectParams bin_effect_params{};
            bin_effect_params.direction = direction;
            bin_effect_params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
            bin_effect_params.spatialBlend = 1.0f;
            bin_effect_params.hrtf = hrtf;

            float* in_data_channels[] = {mono_input_buffer.data()};
            IPLAudioBuffer mono_buffer{};
            mono_buffer.numChannels = 1;
            mono_buffer.numSamples = frame_size;
            mono_buffer.data = in_data_channels;

            size_t processed = 0;
            while (processed < data_count)
            {
                size_t remaining = data_count - processed;
                size_t copy_count = (remaining >= static_cast<size_t>(frame_size))
                                    ? static_cast<size_t>(frame_size) : remaining;

                for (size_t i = 0; i < copy_count; ++i)
                    mono_input_buffer[i] = data[processed + i];
                for (size_t i = copy_count; i < static_cast<size_t>(frame_size); ++i)
                    mono_input_buffer[i] = 0.0f;

                iplBinauralEffectApply(bin_effect, &bin_effect_params, &mono_buffer, &out_buffer);

                for (size_t i = 0; i < copy_count; ++i)
                {
                    output_stereo_buffer[(processed + i) * 2 + 0] = out_buffer.data[0][i] * gain;
                    output_stereo_buffer[(processed + i) * 2 + 1] = out_buffer.data[1][i] * gain;
                }

                processed += frame_size;
            }

            is_playing_finished = true;
        }
    };
    std::map<uint32_t, SoundSource> sound_sources{};

    bool InitSteamAudio()
    {
        IPLContextSettings contextSettings{};
        contextSettings.version = STEAMAUDIO_VERSION;
        if (iplContextCreate(&contextSettings, &context) != IPL_STATUS_SUCCESS)
        {
            fprintf(stderr, "Failed to initialize SteamAudio context!\n");
            return false;
        }
        return true;
    }
    
    bool InitFX()
    {
        audio_settings.samplingRate = 48000;
        audio_settings.frameSize = STEAM_AUDIO_FRAME_SIZE;

        IPLHRTFSettings hrtf_settings{};
        hrtf_settings.type = IPL_HRTFTYPE_DEFAULT;
        hrtf_settings.volume = 1.0f;

        if (iplHRTFCreate(context, &audio_settings, &hrtf_settings, &hrtf) != IPL_STATUS_SUCCESS)
        {
            fprintf(stderr, "Failed to create HRTF!\n");
            return false;
        }

        IPLBinauralEffectSettings bin_effect_settings{};
        bin_effect_settings.hrtf = hrtf;
        if (iplBinauralEffectCreate(context, &audio_settings, &bin_effect_settings, &bin_effect) != IPL_STATUS_SUCCESS)
        {
            fprintf(stderr, "Failed to create binaural effect!\n");
            return false;
        }

        return true;
    }
}
#endif
