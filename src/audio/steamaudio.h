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
    IPLDirectEffect direct_effect = nullptr;
    IPLAudioBuffer out_buffer{};

    class SoundSource
    {
    public:

        IPLVector3 source_direction = {1.0f, 0.0f, 1.0f};
        IPLVector3 source_position = {0.0f, 0.0f, 0.0f};
        IPLVector3 listener_position = {0.0f, 0.0f, 0.0f};
        IPLCoordinateSpace3 source_coordinates{
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, -1.0f},
            listener_position
        };
        
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

            IPLAudioBuffer temp_in_buffer{}, temp_out_buffer{};
            iplAudioBufferAllocate(context, 1, frame_size, &temp_in_buffer);
            iplAudioBufferAllocate(context, 1, frame_size, &temp_out_buffer);

            iplAudioBufferFree(context, &out_buffer);
            iplAudioBufferAllocate(context, 2, frame_size, &out_buffer);

            mono_input_buffer.resize(frame_size);

            float* in_data_channels[] = {mono_input_buffer.data()};
            IPLAudioBuffer in_buffer{};
            in_buffer.numChannels = 1;
            in_buffer.numSamples = frame_size;
            in_buffer.data = in_data_channels;

            IPLDistanceAttenuationModel dist_atten_model{};
            dist_atten_model.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
            dist_atten_model.minDistance = dist_min;
            float distance_atten = iplDistanceAttenuationCalculate(
                context, source_position, listener_position, &dist_atten_model);

            IPLAirAbsorptionModel air_abs_model{};
            air_abs_model.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
            float air_absorption[3];
            iplAirAbsorptionCalculate(context, source_position, listener_position, &air_abs_model, air_absorption);

            IPLDirectivity directivity{};
            directivity.dipoleWeight = 0.5f;
            directivity.dipolePower = 2.0f;
            float directivity_val = iplDirectivityCalculate(
                context, source_coordinates, listener_position, &directivity);

            IPLBinauralEffectParams bin_effect_params{};
            bin_effect_params.direction = source_direction;
            bin_effect_params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
            bin_effect_params.spatialBlend = 1.0f;
            bin_effect_params.hrtf = hrtf;

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

                IPLDirectEffectParams direct_effect_params{};

                direct_effect_params = {};
                direct_effect_params.flags = IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION;
                direct_effect_params.distanceAttenuation = distance_atten;
                iplDirectEffectApply(direct_effect, &direct_effect_params, &in_buffer, &temp_out_buffer);

                direct_effect_params = {};
                direct_effect_params.flags = IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION;
                memcpy(direct_effect_params.airAbsorption, air_absorption, sizeof(air_absorption));
                iplDirectEffectApply(direct_effect, &direct_effect_params, &temp_out_buffer, &temp_in_buffer);

                direct_effect_params = {};
                direct_effect_params.flags = IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY;
                direct_effect_params.directivity = directivity_val;
                iplDirectEffectApply(direct_effect, &direct_effect_params, &temp_in_buffer, &temp_out_buffer);

                direct_effect_params = {};
                direct_effect_params.flags = IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION;
                direct_effect_params.occlusion = 0.4f;
                iplDirectEffectApply(direct_effect, &direct_effect_params, &temp_out_buffer, &temp_in_buffer);

                direct_effect_params = {};
                direct_effect_params.flags = IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION;
                direct_effect_params.transmission[0] = 0.3f;
                direct_effect_params.transmission[1] = 0.2f;
                direct_effect_params.transmission[2] = 0.1f;
                iplDirectEffectApply(direct_effect, &direct_effect_params, &temp_in_buffer, &temp_out_buffer);

                iplBinauralEffectApply(bin_effect, &bin_effect_params, &temp_out_buffer, &out_buffer);

                for (size_t i = 0; i < copy_count; ++i)
                {
                    output_stereo_buffer[(processed + i) * 2 + 0] = out_buffer.data[0][i];
                    output_stereo_buffer[(processed + i) * 2 + 1] = out_buffer.data[1][i];
                }

                processed += frame_size;
            }

            iplAudioBufferFree(context, &temp_in_buffer);
            iplAudioBufferFree(context, &temp_out_buffer);

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

        IPLDirectEffectSettings dir_effect_settings{};
        dir_effect_settings.numChannels = 1; // input and output buffers will have 1 channel
        if (iplDirectEffectCreate(context, &audio_settings, &dir_effect_settings, &direct_effect) != IPL_STATUS_SUCCESS)
        {
            fprintf(stderr, "Failed to create direct effect!\n");
            return false;
        }

        return true;
    }

}
#endif
