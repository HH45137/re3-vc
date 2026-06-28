#pragma once

#ifdef USE_STEAMAUDIO
#include <cstdint>
#include <phonon.h>
#include <stack>
#include <vector>

namespace SteamAudio
{
    constexpr int MAX_SOUND_SOURCE_NUM = 64;
    bool usingSteamAudio = false;
    IPLContext context = nullptr;

    class SoundSource
    {
    public:
        const uint32_t STEAM_AUDIO_FRAME_SIZE = 512;

        IPLVector3 source_direction = {1.0f, 0.0f, 1.0f};
        IPLVector3 source_position = {0.0f, 0.0f, 0.0f};
        IPLVector3 listener_position = {0.0f, 0.0f, 0.0f};
        IPLCoordinateSpace3 source_coordinates{
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, -1.0f},
            listener_position
        };

        float* data = nullptr;
        uint64_t total_frames = 0;
        uint32_t channels = 0;
        uint32_t sample_rate = 0;

        IPLAudioSettings audio_settings{};
        IPLHRTF hrtf = nullptr;
        IPLBinauralEffect bin_effect = nullptr;
        IPLDirectEffect direct_effect = nullptr;
        IPLAudioBuffer out_buffer{};
        std::vector<float> mono_input_buffer;
        bool is_playing_finished = false;
        SoundSource* p_sound_resource = nullptr;
        size_t sample_frame_cursor = 0;

        ~SoundSource()
        {
            if (data)
            {
                free(data);
                data = nullptr;
            }
        }

        bool InitFX()
        {
            audio_settings.samplingRate = this->sample_rate;
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
            if (iplBinauralEffectCreate(context, &audio_settings, &bin_effect_settings, &bin_effect) !=
                IPL_STATUS_SUCCESS)
            {
                fprintf(stderr, "Failed to create binaural effect!\n");
                return false;
            }

            IPLDirectEffectSettings dir_effect_settings{};
            dir_effect_settings.numChannels = 1; // input and output buffers will have 1 channel
            if (iplDirectEffectCreate(context, &audio_settings, &dir_effect_settings, &direct_effect) !=
                IPL_STATUS_SUCCESS)
            {
                fprintf(stderr, "Failed to create direct effect!\n");
                return false;
            }

            iplAudioBufferAllocate(context, 2, audio_settings.frameSize, &out_buffer);
            mono_input_buffer.resize(STEAM_AUDIO_FRAME_SIZE);

            return true;
        }

        void ProcessSpatialAudio(float* output_stereo_buffer, size_t frame_count)
        {
            if (!bin_effect || frame_count != audio_settings.frameSize || !p_sound_resource)
            {
                return;
            }

            for (size_t i = 0; i < frame_count; ++i)
            {
                size_t current_frame = sample_frame_cursor + i;
                if (current_frame < p_sound_resource->total_frames)
                {
                    mono_input_buffer[i] = p_sound_resource->data[current_frame * p_sound_resource->channels];
                }
                else
                {
                    mono_input_buffer[i] = 0.0f;
                }
            }

            float* in_data_channels[] = {mono_input_buffer.data()};
            IPLAudioBuffer in_buffer{};
            in_buffer.numChannels = 1;
            in_buffer.numSamples = static_cast<int32_t>(frame_count);
            in_buffer.data = in_data_channels;

            {
                IPLAudioBuffer temp_in_buffer{}, temp_out_buffer{};
                iplAudioBufferAllocate(context, 1, static_cast<int32_t>(frame_count), &temp_in_buffer);
                iplAudioBufferAllocate(context, 1, static_cast<int32_t>(frame_count), &temp_out_buffer);

                IPLDirectEffectParams direct_effect_params{};

                // 距离衰减
                {
                    IPLDistanceAttenuationModel distance_attenuation_model{};
                    distance_attenuation_model.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
                    float distance_attenuation = iplDistanceAttenuationCalculate(
                        context, source_position, listener_position, &distance_attenuation_model);

                    direct_effect_params = {};
                    direct_effect_params.flags = IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION;
                    direct_effect_params.distanceAttenuation = distance_attenuation;

                    iplDirectEffectApply(direct_effect, &direct_effect_params, &in_buffer, &temp_out_buffer);
                }

                // 空气吸收
                {
                    IPLAirAbsorptionModel air_absorption_model{};
                    air_absorption_model.type = IPL_AIRABSORPTIONTYPE_DEFAULT;

                    direct_effect_params = {};
                    direct_effect_params.flags = IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION;
                    iplAirAbsorptionCalculate(context, source_position, listener_position, &air_absorption_model,
                                              direct_effect_params.airAbsorption);

                    iplDirectEffectApply(direct_effect, &direct_effect_params, &temp_out_buffer, &temp_in_buffer);
                }

                // 方向性
                {
                    IPLDirectivity directivity{};
                    directivity.dipoleWeight = 0.5f;
                    directivity.dipolePower = 2.0f;

                    direct_effect_params = {};
                    direct_effect_params.flags = IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY;
                    direct_effect_params.directivity = iplDirectivityCalculate(
                        context, source_coordinates, listener_position, &directivity);

                    iplDirectEffectApply(direct_effect, &direct_effect_params, &temp_in_buffer, &temp_out_buffer);
                }

                // 阻塞
                {
                    direct_effect_params = {};
                    direct_effect_params.flags = IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION;
                    direct_effect_params.occlusion = 0.4f;

                    iplDirectEffectApply(direct_effect, &direct_effect_params, &temp_out_buffer, &temp_in_buffer);
                }

                // 传输
                {
                    direct_effect_params = {};
                    direct_effect_params.flags = IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION;
                    direct_effect_params.transmission[0] = 0.3f;
                    direct_effect_params.transmission[1] = 0.2f;
                    direct_effect_params.transmission[2] = 0.1f;

                    iplDirectEffectApply(direct_effect, &direct_effect_params, &temp_in_buffer, &temp_out_buffer);
                }

                // 双声道化
                {
                    IPLBinauralEffectParams effect_params{};
                    effect_params.direction = source_direction; // 这里每个音源可以配置不同的 3D 朝向！
                    effect_params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
                    effect_params.spatialBlend = 1.0f;
                    effect_params.hrtf = hrtf;

                    iplBinauralEffectApply(bin_effect, &effect_params, &temp_out_buffer, &out_buffer);
                }

                iplAudioBufferFree(context, &temp_in_buffer);
                iplAudioBufferFree(context, &temp_out_buffer);
            }

            for (size_t i = 0; i < frame_count; ++i)
            {
                output_stereo_buffer[i * 2 + 0] = out_buffer.data[0][i];
                output_stereo_buffer[i * 2 + 1] = out_buffer.data[1][i];
            }

            sample_frame_cursor += frame_count;
            if (sample_frame_cursor >= p_sound_resource->total_frames)
            {
                is_playing_finished = true;
            }
        }
    };

    std::stack<SoundSource> sound_sources;

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

    void AddSoundSource(const SoundSource& audio_src)
    {
        if (sound_sources.size() >= MAX_SOUND_SOURCE_NUM)
        {
            sound_sources.pop();
        }
        sound_sources.push(audio_src);
    }
}
#endif
