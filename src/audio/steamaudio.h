#pragma once

#include <cstdint>
#include <map>
#include <vector>

#ifdef USE_STEAMAUDIO
#include <phonon.h>
#include <cfloat>

namespace SA
{
    static bool usingSteamAudio = false;
    constexpr int MAX_SOUND_SOURCE_NUM = 64;

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

        ~SoundSource();

        void ProcessSpatialAudio(float* output_stereo_buffer, size_t data_count);
    };

    static std::map<uint32_t, SoundSource> sound_sources{};

    bool InitSteamAudio();

    bool InitFX();
}
#endif
