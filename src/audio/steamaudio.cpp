#include "steamaudio.h"

#include <cstdint>
#include <vector>

#ifdef USE_STEAMAUDIO
#include <phonon.h>

namespace SA
{
    const uint32_t STEAM_AUDIO_FRAME_SIZE = 512;

    IPLContext context = nullptr;
    IPLAudioSettings audio_settings{};
    IPLHRTF hrtf = nullptr;
    IPLBinauralEffect bin_effect = nullptr;
    IPLDirectEffect direct_effect = nullptr;

    SoundSource::~SoundSource()
    {
        if (data)
        {
            data = nullptr;
        }
    }

    void SoundSource::ProcessSpatialAudio(float* output_stereo_buffer, size_t data_count)
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
                                    ? static_cast<size_t>(frame_size)
                                    : remaining;

            for (size_t i = 0; i < copy_count; ++i)
                mono_input_buffer[i] = data[processed + i];
            for (size_t i = copy_count; i < static_cast<size_t>(frame_size); ++i)
                mono_input_buffer[i] = 0.0f;

            {
                IPLAudioBuffer temp_in_buffer{}, temp_out_buffer{};
                iplAudioBufferAllocate(context, 1, frame_size, &temp_in_buffer);
                iplAudioBufferAllocate(context, 1, frame_size, &temp_out_buffer);

                IPLDirectEffectParams direct_effect_params{};
                direct_effect_params.flags = static_cast<IPLDirectEffectFlags>(direct_effect_params.flags |
                    IPL_DISTANCEATTENUATIONTYPE_DEFAULT);
                direct_effect_params.flags = static_cast<IPLDirectEffectFlags>(direct_effect_params.flags |
                    IPL_AIRABSORPTIONTYPE_DEFAULT);
                direct_effect_params.flags = static_cast<IPLDirectEffectFlags>(direct_effect_params.flags |
                    IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY);
                direct_effect_params.flags = static_cast<IPLDirectEffectFlags>(direct_effect_params.flags |
                    IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION);
                direct_effect_params.flags = static_cast<IPLDirectEffectFlags>(direct_effect_params.flags |
                    IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);

                // 距离衰减
                {
                    IPLDistanceAttenuationModel distance_attenuation_model{};
                    distance_attenuation_model.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
                    float distance_attenuation = iplDistanceAttenuationCalculate(
                        context, source_position, listener_position, &distance_attenuation_model);

                    direct_effect_params.distanceAttenuation = distance_attenuation;
                }

                // 空气吸收
                {
                    IPLAirAbsorptionModel air_absorption_model{};
                    air_absorption_model.type = IPL_AIRABSORPTIONTYPE_DEFAULT;

                    iplAirAbsorptionCalculate(context, source_position, listener_position, &air_absorption_model,
                                              direct_effect_params.airAbsorption);
                }

                // 方向性
                {
                    IPLCoordinateSpace3 source_coordinates{
                        {1.0f, 0.0f, 0.0f},
                        {0.0f, 1.0f, 0.0f},
                        {0.0f, 0.0f, -1.0f},
                        source_position
                    };

                    IPLDirectivity directivity{};
                    directivity.dipoleWeight = 0.5f;
                    directivity.dipolePower = 2.0f;

                    direct_effect_params.directivity = iplDirectivityCalculate(
                        context, source_coordinates, listener_position, &directivity);
                }

                // 阻塞
                {
                    direct_effect_params.occlusion = 1.0f;
                }

                // 传输
                {
                    direct_effect_params.transmission[0] = 1.0f;
                    direct_effect_params.transmission[1] = 1.0f;
                    direct_effect_params.transmission[2] = 1.0f;
                }

                // Final apply direct effects
                iplDirectEffectApply(direct_effect, &direct_effect_params, &mono_buffer, &temp_out_buffer);

                // 双声道化
                {
                    IPLBinauralEffectParams bin_effect_params{};
                    bin_effect_params.direction = direction;
                    bin_effect_params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
                    bin_effect_params.spatialBlend = 1.0f;
                    bin_effect_params.hrtf = hrtf;
                    iplBinauralEffectApply(bin_effect, &bin_effect_params, &temp_out_buffer, &out_buffer);
                }

                iplAudioBufferFree(context, &temp_in_buffer);
                iplAudioBufferFree(context, &temp_out_buffer);
            }

            for (size_t i = 0; i < copy_count; ++i)
            {
                output_stereo_buffer[(processed + i) * 2 + 0] = out_buffer.data[0][i] * gain;
                output_stereo_buffer[(processed + i) * 2 + 1] = out_buffer.data[1][i] * gain;
            }

            processed += frame_size;
        }

        is_playing_finished = true;
    }

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
