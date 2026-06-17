#pragma once
#include <vector>
#include <array>
#include <atomic>
#include <iostream>
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#include <phonon.h>


namespace RTA {
    struct SoundData {
        float *data = nullptr;
        uint64 total_frames = 0;
        uint32_t channels = 0;
        uint32_t sample_rate = 0;

        ~SoundData() {
            if (data) {
                free(data);
                data = nullptr;
            }
        }
    };

    class IAudioBackend {
    public:
        virtual ~IAudioBackend() = default;

        virtual bool Initialize() = 0;

        virtual void Shutdown() = 0;

        virtual bool IsReady() = 0;

        virtual void Play() = 0;

        virtual void Pause() = 0;

        virtual void Reset() = 0;

        virtual bool LoadDataFromFile(const std::string &_path) = 0;

        virtual size_t GetDurationInMilliseconds() = 0;
    };

    class RTAudio : public IAudioBackend {
    public:
        IPLVector3 source_direction = {1.0f, 0.0f, 1.0f};
        IPLVector3 source_position = {0.0f, 0.0f, 0.0f};
        IPLVector3 listener_position = {0.0f, 0.0f, 0.0f};

        ~RTAudio() override {
            RTAudio::Shutdown();
        }

        bool Initialize() override {
            is_playing_finished = false;
            sample_frame_cursor = 0;
            device_config = {};
            device = {};

            IPLContextSettings contextSettings{};
            contextSettings.version = STEAMAUDIO_VERSION;
            if (iplContextCreate(&contextSettings, &context) != IPL_STATUS_SUCCESS) {
                std::cout << "Failed to initialize SteamAudio context!\n";
                return false;
            }

            is_ready = true;
            return true;
        }

        void Shutdown() override {
            if (!is_ready) { return; }

            ma_device_uninit(&this->device);
            if (bin_effect) {
                iplBinauralEffectRelease(&bin_effect);
            }
            if (hrtf) {
                iplHRTFRelease(&hrtf);
            }
            if (context) {
                iplContextRelease(&context);
            }

            p_sound_resource = nullptr;

            if (out_buffer.data) {
                iplAudioBufferFree(context, &out_buffer);
            }

            this->is_ready = false;
        }

        bool IsReady() override {
            return is_ready;
        }

        void Play() override {
            if (!this->is_ready || p_sound_resource == nullptr) {
                return;
            }
            if (is_playing_finished) {
                this->Reset();
            }
            if (ma_device_start(&this->device) != MA_SUCCESS) {
                std::cout << "Failed to start playback device.\n";
            }
        }

        void Pause() override {
            if (!this->is_ready || p_sound_resource == nullptr) {
                return;
            }
            ma_device_stop(&this->device);
        }

        void Reset() override {
            if (!this->is_ready || p_sound_resource == nullptr) {
                return;
            }
            ma_device_stop(&this->device);
            sample_frame_cursor = 0;
            is_playing_finished = false;
        }

        // 核心修改：为了不破坏虚函数接口，这里在内部用一个静态 Map 或是单纯的临时加载。
        // 如果您想实现优雅的多音源，更推荐外部传入 SoundData 指针，这里作为演示演示如何不改接口实现。
        bool LoadDataFromFile(const std::string &_path) override {
            // 允许外部加载。为了保证不重复加载浪费内存，可以由外部读完通过别的方法传进来。
            // 如果这里直接读，我们认为外部保证每个路径对应的 Sound 独立。
            uint32_t file_channels = 0;
            uint32_t file_sample_rate = 0;
            uint64_t file_total_frames = 0;

            float *loaded_data = nullptr;
            // loaded_data = drwav_open_file_and_read_pcm_frames_f32(_path.c_str(), &file_channels, &file_sample_rate, &file_total_frames, nullptr);
            if (loaded_data == nullptr) {
                std::cout << "Error opening and reading WAV file.\n";
                return false;
            }

            // 把数据托管给一个动态分配的结构体（防止多个实例冲突）
            current_managed_sound = std::make_unique<SoundData>();
            current_managed_sound->data = loaded_data;
            current_managed_sound->channels = file_channels;
            current_managed_sound->sample_rate = file_sample_rate;
            current_managed_sound->total_frames = file_total_frames;

            // 让当前音源实例指向这块数据资产
            p_sound_resource = current_managed_sound.get();

            constexpr uint32_t STEAM_AUDIO_FRAME_SIZE = 512;
            device_config = ma_device_config_init(ma_device_type_playback);
            device_config.playback.format = ma_format_f32;
            device_config.playback.channels = 2; // HRTF 空间化输出固定为立体声双声道
            device_config.sampleRate = p_sound_resource->sample_rate;
            device_config.periodSizeInFrames = STEAM_AUDIO_FRAME_SIZE;
            device_config.dataCallback = data_callback;
            device_config.pUserData = this;

            if (ma_device_init(nullptr, &this->device_config, &this->device) != MA_SUCCESS) {
                std::cout << "Failed to open playback device.\n";
                return false;
            }

            audio_settings.samplingRate = static_cast<int32_t>(p_sound_resource->sample_rate);
            audio_settings.frameSize = STEAM_AUDIO_FRAME_SIZE;

            IPLHRTFSettings hrtf_settings{};
            hrtf_settings.type = IPL_HRTFTYPE_DEFAULT;
            hrtf_settings.volume = 1.0f;

            if (iplHRTFCreate(context, &audio_settings, &hrtf_settings, &hrtf) != IPL_STATUS_SUCCESS) {
                std::cout << "Failed to create HRTF!\n";
                return false;
            }

            IPLBinauralEffectSettings bin_effect_settings{};
            bin_effect_settings.hrtf = hrtf;
            if (iplBinauralEffectCreate(context, &audio_settings, &bin_effect_settings, &bin_effect) != IPL_STATUS_SUCCESS) {
                std::cout << "Failed to create binaural effect!\n";
                return false;
            }

            IPLDirectEffectSettings dir_effect_settings{};
            dir_effect_settings.numChannels = 1; // input and output buffers will have 1 channel
            if (iplDirectEffectCreate(context, &audio_settings, &dir_effect_settings, &direct_effect) != IPL_STATUS_SUCCESS) {
                std::cout << "Failed to create direct effect!\n";
                return false;
            }

            iplAudioBufferAllocate(context, 2, audio_settings.frameSize, &out_buffer);
            mono_input_buffer.resize(STEAM_AUDIO_FRAME_SIZE);

            return true;
        }

        size_t GetDurationInMilliseconds() override {
            if (!p_sound_resource || p_sound_resource->sample_rate == 0) {
                return 0;
            }
            return (p_sound_resource->total_frames / p_sound_resource->sample_rate) * 1000;
        }

    private:
        bool is_ready = false;
        std::atomic<bool> is_playing_finished{false};

        SoundData *p_sound_resource = nullptr;
        std::unique_ptr<SoundData> current_managed_sound = nullptr; // 用于 LoadDataFromFile 的生命周期持有

        size_t sample_frame_cursor = 0; // 每个实例独享自己的播放游标

        ma_device_config device_config = {};
        ma_device device = {};

        IPLContext context = nullptr;
        IPLAudioSettings audio_settings{};
        IPLHRTF hrtf = nullptr;
        IPLBinauralEffect bin_effect = nullptr;
        IPLDirectEffect direct_effect = nullptr;

        IPLAudioBuffer out_buffer{};
        std::vector<float> mono_input_buffer;

        IPLCoordinateSpace3 source_coordinates{
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, -1.0f},
            listener_position
        };

        void ProcessSpatialAudio(float *output_stereo_buffer, size_t frame_count) {
            if (!bin_effect || frame_count != audio_settings.frameSize || !p_sound_resource) {
                return;
            }

            for (size_t i = 0; i < frame_count; ++i) {
                size_t current_frame = sample_frame_cursor + i;
                if (current_frame < p_sound_resource->total_frames) {
                    // 读取自己对应的独立音频资产
                    mono_input_buffer[i] = p_sound_resource->data[current_frame * p_sound_resource->channels];
                } else {
                    mono_input_buffer[i] = 0.0f;
                }
            }

            float *in_data_channels[] = {mono_input_buffer.data()};
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
                    float distance_attenuation = iplDistanceAttenuationCalculate(context, source_position, listener_position, &distance_attenuation_model);

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
                    iplAirAbsorptionCalculate(context, source_position, listener_position, &air_absorption_model, direct_effect_params.airAbsorption);

                    iplDirectEffectApply(direct_effect, &direct_effect_params, &temp_out_buffer, &temp_in_buffer);
                }

                // 方向性
                {
                    IPLDirectivity directivity{};
                    directivity.dipoleWeight = 0.5f;
                    directivity.dipolePower = 2.0f;

                    direct_effect_params = {};
                    direct_effect_params.flags = IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY;
                    direct_effect_params.directivity = iplDirectivityCalculate(context, source_coordinates, listener_position, &directivity);

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

            for (size_t i = 0; i < frame_count; ++i) {
                output_stereo_buffer[i * 2 + 0] = out_buffer.data[0][i];
                output_stereo_buffer[i * 2 + 1] = out_buffer.data[1][i];
            }

            sample_frame_cursor += frame_count;
            if (sample_frame_cursor >= p_sound_resource->total_frames) {
                is_playing_finished = true;
            }
        }

        static void data_callback(ma_device *_device, void *_output, const void *_input, ma_uint32 _frame_count) {
            auto *const this_ptr = static_cast<RTAudio *>(_device->pUserData);
            if (!this_ptr || this_ptr->is_playing_finished) {
                memset(_output, 0, _frame_count * _device->playback.channels * sizeof(float));
                return;
            }
            this_ptr->ProcessSpatialAudio(static_cast<float *>(_output), _frame_count);
        }
    };

}
