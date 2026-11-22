#pragma once

#include "Libretro.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

class Core {
public:
    explicit Core(const std::filesystem::path& library);
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    void load(const std::filesystem::path& path, std::vector<uint8_t> rom,
              const std::filesystem::path& systemDir, const std::filesystem::path& saveDir);
    void run(uint16_t buttons);
    void reset();

    struct Video {
        std::vector<uint32_t> pixels;
        unsigned width = 0;
        unsigned height = 0;
    };

    const Video& video() const { return video_; }
    const std::vector<int16_t>& audio() const { return audio_; }
    const retro::AVInfo& av() const { return av_; }
    bool shutdownRequested() const { return shutdown_; }

    std::vector<uint8_t> readMemory(unsigned id) const;
    void writeMemory(unsigned id, const std::vector<uint8_t>& data);
    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& data);

private:
    template <typename T> void bind(T& destination, const char* name);
    void receiveVideo(const void* data, unsigned width, unsigned height, size_t pitch);
    bool environment(unsigned command, void* data);
    void checkCallbacks() const;

    static bool onEnvironment(unsigned command, void* data) noexcept;
    static void onVideo(const void* data, unsigned width, unsigned height, size_t pitch) noexcept;
    static void onAudio(int16_t left, int16_t right) noexcept;
    static size_t onAudioBatch(const int16_t* data, size_t frames) noexcept;
    static int16_t onInputState(unsigned port, unsigned device, unsigned index, unsigned id);
    static Core* active_;

    void* module_ = nullptr;
    void (*setEnvironment_)(retro::EnvironmentCallback) = nullptr;
    void (*setVideo_)(retro::VideoCallback) = nullptr;
    void (*setAudio_)(retro::AudioCallback) = nullptr;
    void (*setAudioBatch_)(retro::AudioBatchCallback) = nullptr;
    void (*setInputPoll_)(retro::InputPollCallback) = nullptr;
    void (*setInputState_)(retro::InputStateCallback) = nullptr;
    void (*getSystemInfo_)(retro::SystemInfo*) = nullptr;
    void (*getAVInfo_)(retro::AVInfo*) = nullptr;
    void (*setController_)(unsigned, unsigned) = nullptr;
    unsigned (*apiVersion_)() = nullptr;
    void (*init_)() = nullptr;
    void (*deinit_)() = nullptr;
    bool (*loadGame_)(const retro::GameInfo*) = nullptr;
    void (*unloadGame_)() = nullptr;
    void (*run_)() = nullptr;
    void (*reset_)() = nullptr;
    void* (*getMemoryData_)(unsigned) = nullptr;
    size_t (*getMemorySize_)(unsigned) = nullptr;
    size_t (*serializeSize_)() = nullptr;
    bool (*serialize_)(void*, size_t) = nullptr;
    bool (*unserialize_)(const void*, size_t) = nullptr;

    bool initialized_ = false;
    bool loaded_ = false;
    bool shutdown_ = false;
    bool callbackFailed_ = false;
    retro::AVInfo av_{};
    retro::PixelFormat pixelFormat_ = retro::RGB1555;
    Video video_;
    std::vector<int16_t> audio_;
    std::vector<uint8_t> rom_;
    std::string romPath_;
    std::string systemDir_;
    std::string saveDir_;
    std::map<std::string, std::string> variables_;
    uint16_t buttons_ = 0;
};
