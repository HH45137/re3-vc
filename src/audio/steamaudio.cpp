#include "steamaudio.h"

#include <cstdint>
#include <vector>
#include <cmath>

#ifdef USE_STEAMAUDIO
#include <phonon.h>

namespace SA
{
    IPLContext context = nullptr;
    IPLAudioSettings audio_settings{};
    IPLHRTF hrtf = nullptr;
    IPLBinauralEffect bin_effect = nullptr;
    IPLDirectEffect direct_effect = nullptr;

    bool usingSteamAudio = false;
    std::map<uint32_t, SoundSource> sound_sources{};

    // Shared listener state, already converted to Steam Audio space.
    IPLVector3 listener_position = {0.0f, 0.0f, 0.0f};
    IPLVector3 listener_ahead    = {0.0f, 0.0f, 1.0f};
    IPLVector3 listener_up       = {0.0f, 1.0f, 0.0f};

    void UpdateListener(float posX, float posY, float posZ,
                        float aheadX, float aheadY, float aheadZ,
                        float upX, float upY, float upZ)
    {
        listener_position = GameToIPL(posX, posY, posZ);
        listener_ahead    = GameToIPL(aheadX, aheadY, aheadZ);
        listener_up       = GameToIPL(upX, upY, upZ);
    }

    SoundSource::~SoundSource()
    {
        if (data)
        {
            data = nullptr;
        }
        if (effect)
        {
            iplBinauralEffectRelease(&effect);
            effect = nullptr;
        }
    }

    void SoundSource::ProcessSpatialAudio(float* output_stereo_buffer, size_t data_count)
    {
        if (!bin_effect || !data)
        {
            return;
        }

        // 2D sounds (radio, UI, police radio, ...): no spatialization at all,
        // just a mono->stereo copy with pan applied as channel balance.
        if (is2d)
        {
            const float balance = (pan - 63.0f) / 64.0f; // -1 .. 1
            float left_gain = gain * (1.0f - balance); if (left_gain < 0.0f) left_gain = 0.0f;
            float right_gain = gain * (1.0f + balance); if (right_gain < 0.0f) right_gain = 0.0f;
            if (left_gain > 2.0f) left_gain = 2.0f;
            if (right_gain > 2.0f) right_gain = 2.0f;
            for (size_t i = 0; i < data_count; ++i)
            {
                output_stereo_buffer[i * 2 + 0] = data[i] * left_gain;
                output_stereo_buffer[i * 2 + 1] = data[i] * right_gain;
            }
            is_playing_finished = true;
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

            // NOTE: loudness/distance attenuation is already handled by the game
            // itself: cAudioManager::ComputeVolume() bakes distance attenuation
            // into the channel volume (CChannel::SetVolume -> gain). Applying
            // Steam Audio's distance attenuation here as well would attenuate
            // twice, making all 3D sounds nearly inaudible. Steam Audio is
            // therefore only used for binaural (directional) spatialization.
            {
                IPLBinauralEffectParams bin_effect_params{};
                bin_effect_params.direction = direction;
                bin_effect_params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
                bin_effect_params.spatialBlend = 1.0f;
                bin_effect_params.hrtf = hrtf;
                iplBinauralEffectApply(bin_effect, &bin_effect_params, &mono_buffer, &out_buffer);
            }

            for (size_t i = 0; i < copy_count; ++i)
            {
                output_stereo_buffer[(processed + i) * 2 + 0] = out_buffer.data[0][i] * gain;
                output_stereo_buffer[(processed + i) * 2 + 1] = out_buffer.data[1][i] * gain;
            }

            processed += copy_count;
        }

        is_playing_finished = true;
    }

    void SpatializeBlock(SoundSource& src, const float* in, size_t frames, int16_t* out_stereo)
    {
        if (frames == 0) return;
        const int32_t frame_size = audio_settings.frameSize;
        if (frames > static_cast<size_t>(frame_size))
            frames = static_cast<size_t>(frame_size);

        if (!bin_effect || !hrtf)
        {
            // Fallback: plain mono -> stereo copy.
            for (size_t i = 0; i < frames; ++i)
            {
                float v = in[i];
                if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
                out_stereo[i * 2 + 0] = out_stereo[i * 2 + 1] = static_cast<int16_t>(v * 32767.0f);
            }
            return;
        }

        // Each source gets its own stateful binaural effect (lazily created,
        // released in ~SoundSource). The global bin_effect is only used as an
        // "initialized" indicator here.
        if (!src.effect)
        {
            IPLBinauralEffectSettings effect_settings{};
            effect_settings.hrtf = hrtf;
            if (iplBinauralEffectCreate(context, &audio_settings, &effect_settings, &src.effect) != IPL_STATUS_SUCCESS)
            {
                src.effect = nullptr;
                for (size_t i = 0; i < frames; ++i)
                {
                    float v = in[i];
                    if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
                    out_stereo[i * 2 + 0] = out_stereo[i * 2 + 1] = static_cast<int16_t>(v * 32767.0f);
                }
                return;
            }
        }

        // Lazily allocated scratch buffers, reused for every block.
        static IPLAudioBuffer mono_scratch{};
        static IPLAudioBuffer stereo_scratch{};
        if (stereo_scratch.data == nullptr)
        {
            iplAudioBufferAllocate(context, 1, frame_size, &mono_scratch);
            iplAudioBufferAllocate(context, 2, frame_size, &stereo_scratch);
        }

        float* mono = mono_scratch.data[0];
        for (size_t i = 0; i < frames; ++i)
            mono[i] = in[i];
        for (auto i = frames; i < static_cast<size_t>(frame_size); ++i)
            mono[i] = 0.0f;

        // Direction relative to the CURRENT positions of source and listener,
        // smoothed so the HRTF filter changes gradually instead of jumping
        // at every block boundary (which is heard as crackle).
        IPLVector3 target = iplCalculateRelativeDirection(
            context, src.source_position, listener_position, listener_ahead, listener_up);
        if (!src.direction_initialized)
        {
            src.render_direction = target;
            src.direction_initialized = true;
        }
        else
        {
            // Fast smoothing (~25ms time constant). If the direction moved
            // more than 90 degrees in one step (e.g. fast camera spin or a
            // sound jumping behind the listener), snap instead of lerping:
            // a straight lerp between near-opposite unit vectors passes
            // through the origin and makes the normalized direction drift,
            // which is heard as unstable/wandering direction.
            float dot = src.render_direction.x * target.x +
                        src.render_direction.y * target.y +
                        src.render_direction.z * target.z;
            if (dot < 0.0f)
            {
                src.render_direction = target;
            }
            else
            {
                // Fast smoothing (~20ms time constant). 1.0 (no smoothing)
                // makes the HRTF filter jump at every block boundary, which
                // is heard as constant crackle; 0.75 is fast enough to feel
                // instantaneous while keeping the filter continuous.
                const float k = 0.75f;
                src.render_direction.x += (target.x - src.render_direction.x) * k;
                src.render_direction.y += (target.y - src.render_direction.y) * k;
                src.render_direction.z += (target.z - src.render_direction.z) * k;
                float len = std::sqrt(src.render_direction.x * src.render_direction.x +
                                      src.render_direction.y * src.render_direction.y +
                                      src.render_direction.z * src.render_direction.z);
                if (len > 1e-6f)
                {
                    src.render_direction.x /= len;
                    src.render_direction.y /= len;
                    src.render_direction.z /= len;
                }
            }
        }

        IPLBinauralEffectParams bin_effect_params{};
        bin_effect_params.direction = src.render_direction;
        bin_effect_params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
        bin_effect_params.spatialBlend = 1.0f;
        bin_effect_params.hrtf = hrtf;
        iplBinauralEffectApply(src.effect, &bin_effect_params, &mono_scratch, &stereo_scratch);

        for (size_t i = 0; i < frames; ++i)
        {
            float l = stereo_scratch.data[0][i];
            float r = stereo_scratch.data[1][i];
            if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
            out_stereo[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
            out_stereo[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
        }
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
        audio_settings.samplingRate = STEAM_AUDIO_SAMPLING_RATE; // must match the OpenAL device rate
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
