#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <vector>

#ifdef USE_STEAMAUDIO
#include <phonon.h>
#include <cfloat>

namespace SA
{
    extern bool usingSteamAudio;
    constexpr int MAX_SOUND_SOURCE_NUM = 64;
    constexpr uint32_t STEAM_AUDIO_FRAME_SIZE = 512;

    // Steam Audio processing rate. MUST match the OpenAL device sample rate
    // (see ALC_FREQUENCY in sampman_sa.cpp, which uses this constant) so the
    // streamed blocks are played back 1:1 without any resampling by OpenAL.
    // 48 kHz also preserves the default HRTF's high-frequency localization
    // cues; running the HRTF at 32 kHz made direction cues almost inaudible.
    constexpr uint32_t STEAM_AUDIO_SAMPLING_RATE = 48000;

    // Coordinate space conversion:
    //   GTA III/VC world: right-handed, +X east, +Y north, +Z up.
    //   Steam Audio:      left-handed,  +X right, +Y up,   +Z forward.
    // The mapping (x, y, z) -> (-x, z, y) is a reflection that converts
    // between the two conventions. It must be applied to ALL positions
    // and direction vectors (source, listener, ahead, up) alike.
    inline IPLVector3 GameToIPL(float gx, float gy, float gz)
    {
        IPLVector3 v;
        v.x = -gx;
        v.y = gz;
        v.z = gy;
        return v;
    }

    // Single shared listener state, updated once per frame via UpdateListener().
    // (Previously every SoundSource cached its own copy captured at Start()
    // time, which made the sound field inconsistent between sources.)
    void UpdateListener(float posX, float posY, float posZ,
                        float aheadX, float aheadY, float aheadZ,
                        float upX, float upY, float upZ);

    class SoundSource
    {
    public:
        IPLVector3 source_position = {0.0f, 0.0f, 0.0f}; // Steam Audio space
        // Smoothed direction actually used for rendering. Recomputed from the
        // current positions per block and exponentially interpolated, so the
        // HRTF filter never jumps abruptly between blocks (zipper noise).
        IPLVector3 render_direction = {0.0f, 0.0f, 1.0f};
        bool direction_initialized = false;
        bool is2d = false;          // 2D sounds bypass spatialization
        float pan = 63.0f;          // 0..127, only used for 2D sounds
        float min_distance = 1.0f;  // full volume within this distance
        float max_distance = 50.0f;

        IPLAudioBuffer out_buffer{};
        float gain = 1.0f;

        // Each source owns its own binaural effect: the effect is STATEFUL
        // (filter tail, see iplBinauralEffectReset/GetTailSize). Sharing one
        // effect across interleaved channels corrupted the filter state and
        // caused constant crackle on 3D sounds.
        IPLBinauralEffect effect = nullptr;

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

    // Spatialize a single block (<= STEAM_AUDIO_FRAME_SIZE frames) of mono
    // audio into interleaved stereo int16, using the source's CURRENT
    // position and the shared, per-frame updated listener state. The
    // direction is smoothed inside src to avoid HRTF discontinuities.
    // Gain is NOT applied here - loudness is controlled live via the AL
    // source gain.
    void SpatializeBlock(SoundSource& src, const float* in, size_t frames, int16_t* out_stereo);

    extern std::map<uint32_t, SoundSource> sound_sources;

    bool InitSteamAudio();

    bool InitFX();
}
#endif
