#include "Core.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
constexpr size_t maxDataSize = 32 * 1024 * 1024;
constexpr size_t maxAudioSamples = 32 * 1024;

void* loadModule(const std::filesystem::path& path)
{
#ifdef _WIN32
    return LoadLibraryExW(path.c_str(), nullptr,
                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void closeModule(void* module)
{
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(module));
#else
    dlclose(module);
#endif
}

bool validGeometry(const retro::Geometry& geometry)
{
    return geometry.baseWidth && geometry.baseHeight && geometry.maxWidth <= 2048 &&
           geometry.maxHeight <= 2048 && geometry.maxWidth >= geometry.baseWidth &&
           geometry.maxHeight >= geometry.baseHeight && std::isfinite(geometry.aspectRatio);
}

bool validAV(const retro::AVInfo& av)
{
    return validGeometry(av.geometry) && av.timing.fps >= 1 && av.timing.fps <= 1000 &&
           av.timing.sampleRate >= 1000 && av.timing.sampleRate <= 384000;
}

uint32_t expandColor(unsigned value, unsigned max)
{
    return value * 255 / max;
}
}

Core* Core::active_ = nullptr;

template <typename T>
void Core::bind(T& destination, const char* name)
{
#ifdef _WIN32
    auto address = GetProcAddress(static_cast<HMODULE>(module_), name);
#else
    auto address = dlsym(module_, name);
#endif
    if (!address) {
        throw std::runtime_error(std::string("The DLL is missing ") + name + ".");
    }
    static_assert(sizeof(address) == sizeof(destination));
    std::memcpy(&destination, &address, sizeof(address));
}

Core::Core(const std::filesystem::path& library)
{
    if (active_) {
        throw std::runtime_error("Close the current game before opening another.");
    }

    audio_.reserve(maxAudioSamples);
    module_ = loadModule(std::filesystem::absolute(library));
    if (!module_) {
#ifdef _WIN32
        const auto error = GetLastError();
        throw std::runtime_error("Could not load the Gambatte DLL (Windows error " +
                                 std::to_string(error) + "). Check that it is a Windows x64 core.");
#else
        throw std::runtime_error(dlerror());
#endif
    }

    try {
        bind(apiVersion_, "retro_api_version");
        bind(getSystemInfo_, "retro_get_system_info");
        if (apiVersion_() != retro::apiVersion) {
            throw std::runtime_error("The DLL uses an unsupported libretro API version.");
        }

        retro::SystemInfo info{};
        getSystemInfo_(&info);
        if (!info.libraryName || std::strcmp(info.libraryName, "Gambatte") != 0 || info.needFullpath) {
            throw std::runtime_error("Select an extracted Windows x64 Gambatte libretro core.");
        }

        bind(setEnvironment_, "retro_set_environment");
        bind(setVideo_, "retro_set_video_refresh");
        bind(setAudio_, "retro_set_audio_sample");
        bind(setAudioBatch_, "retro_set_audio_sample_batch");
        bind(setInputPoll_, "retro_set_input_poll");
        bind(setInputState_, "retro_set_input_state");
        bind(init_, "retro_init");
        bind(deinit_, "retro_deinit");
        bind(loadGame_, "retro_load_game");
        bind(unloadGame_, "retro_unload_game");
        bind(getAVInfo_, "retro_get_system_av_info");
        bind(setController_, "retro_set_controller_port_device");
        bind(run_, "retro_run");
        bind(reset_, "retro_reset");
        bind(getMemoryData_, "retro_get_memory_data");
        bind(getMemorySize_, "retro_get_memory_size");
        bind(serializeSize_, "retro_serialize_size");
        bind(serialize_, "retro_serialize");
        bind(unserialize_, "retro_unserialize");
    } catch (...) {
        closeModule(module_);
        throw;
    }

    active_ = this;
}

Core::~Core()
{
    if (loaded_) {
        unloadGame_();
    }
    if (initialized_) {
        deinit_();
    }
    closeModule(module_);
    active_ = nullptr;
}

void Core::load(const std::filesystem::path& path, std::vector<uint8_t> rom,
                const std::filesystem::path& systemDir, const std::filesystem::path& saveDir)
{
    if (initialized_ || rom.size() < 32 * 1024 || rom.size() > maxDataSize) {
        throw std::runtime_error("The game could not be loaded. Select a complete Game Boy or Game Boy Color ROM.");
    }

    rom_ = std::move(rom);
    romPath_ = path.u8string();
    systemDir_ = systemDir.u8string();
    saveDir_ = saveDir.u8string();
    setEnvironment_(onEnvironment);
    setVideo_(onVideo);
    setAudio_(onAudio);
    setAudioBatch_(onAudioBatch);
    setInputPoll_([] {});
    setInputState_(onInputState);

    init_();
    initialized_ = true;

    const retro::GameInfo game{romPath_.c_str(), rom_.data(), rom_.size(), nullptr};
    loaded_ = loadGame_(&game);
    if (!loaded_) {
        throw std::runtime_error("Gambatte could not load this ROM.");
    }

    setController_(0, retro::joypad);
    getAVInfo_(&av_);
    checkCallbacks();
    if (!validAV(av_)) {
        throw std::runtime_error("Gambatte reported invalid video or audio settings.");
    }
}

void Core::run(uint16_t buttons)
{
    buttons_ = buttons;
    audio_.clear();
    run_();
    checkCallbacks();
}

void Core::reset()
{
    reset_();
    checkCallbacks();
}

std::vector<uint8_t> Core::readMemory(unsigned id) const
{
    const auto size = getMemorySize_(id);
    const auto* memory = static_cast<const uint8_t*>(getMemoryData_(id));
    if (!size) {
        return {};
    }
    if (!memory || size > maxDataSize) {
        throw std::runtime_error("Gambatte reported invalid battery-backed memory.");
    }
    return {memory, memory + size};
}

void Core::writeMemory(unsigned id, const std::vector<uint8_t>& data)
{
    auto* memory = getMemoryData_(id);
    if (!memory || getMemorySize_(id) != data.size()) {
        throw std::runtime_error("The battery save size does not match this ROM.");
    }
    std::memcpy(memory, data.data(), data.size());
}

std::vector<uint8_t> Core::saveState() const
{
    const auto size = serializeSize_();
    if (!size || size > maxDataSize) {
        throw std::runtime_error("Gambatte reported an invalid save state size.");
    }
    std::vector<uint8_t> data(size);
    if (!serialize_(data.data(), data.size())) {
        throw std::runtime_error("Gambatte could not create a save state.");
    }
    return data;
}

void Core::loadState(const std::vector<uint8_t>& data)
{
    if (data.empty() || data.size() != serializeSize_() || !unserialize_(data.data(), data.size())) {
        throw std::runtime_error("Gambatte could not restore this save state.");
    }
}

bool Core::environment(unsigned command, void* data)
{
    if (command == retro::Shutdown) {
        shutdown_ = true;
        return true;
    }
    if (command == retro::GetInputBitmasks || command == retro::SetInputDescriptors) {
        return true;
    }
    if (!data) {
        return false;
    }

    switch (command) {
    case retro::GetCanDupe:
        *static_cast<bool*>(data) = true;
        return true;
    case retro::SystemDirectory:
        *static_cast<const char**>(data) = systemDir_.c_str();
        return true;
    case retro::GetSaveDirectory:
        *static_cast<const char**>(data) = saveDir_.c_str();
        return true;
    case retro::GetCoreOptionsVersion:
    case retro::GetLanguage:
        *static_cast<unsigned*>(data) = 0;
        return true;
    case retro::SetPixelFormat: {
        const auto format = *static_cast<const retro::PixelFormat*>(data);
        if (format != retro::RGB1555 && format != retro::XRGB8888 && format != retro::RGB565) {
            return false;
        }
        pixelFormat_ = format;
        return true;
    }
    case retro::SetSystemAVInfo: {
        const auto& av = *static_cast<const retro::AVInfo*>(data);
        if (!validAV(av)) {
            return false;
        }
        av_ = av;
        return true;
    }
    case retro::SetGeometry: {
        // SET_GEOMETRY only changes the base dimensions and aspect ratio.
        const auto& requested = *static_cast<const retro::Geometry*>(data);
        auto geometry = av_.geometry;
        geometry.baseWidth = requested.baseWidth;
        geometry.baseHeight = requested.baseHeight;
        geometry.aspectRatio = requested.aspectRatio;
        if (!validGeometry(geometry)) {
            return false;
        }
        av_.geometry = geometry;
        return true;
    }
    case retro::SetVariables: {
        const auto* variable = static_cast<const retro::Variable*>(data);
        for (; variable->key; ++variable) {
            if (!variable->value) {
                continue;
            }
            const std::string choices = variable->value;
            const auto separator = choices.find("; ");
            if (separator != std::string::npos) {
                const auto start = separator + 2;
                const auto end = choices.find('|', start);
                variables_.try_emplace(variable->key, choices.substr(start, end - start));
            }
        }
        return true;
    }
    case retro::GetVariable: {
        auto& variable = *static_cast<retro::Variable*>(data);
        const auto option = variable.key ? variables_.find(variable.key) : variables_.end();
        variable.value = option == variables_.end() ? nullptr : option->second.c_str();
        return variable.value != nullptr;
    }
    case retro::GetVariableUpdate:
        *static_cast<bool*>(data) = false;
        return true;
    default:
        return false;
    }
}

void Core::receiveVideo(const void* data, unsigned width, unsigned height, size_t pitch)
{
    if (!data) {
        return;
    }

    const size_t pixelSize = pixelFormat_ == retro::XRGB8888 ? 4 : 2;
    if (!width || !height || width > 2048 || height > 2048 || pitch < width * pixelSize) {
        throw std::runtime_error("Invalid video frame.");
    }

    video_.pixels.resize(size_t(width) * height);
    video_.width = width;
    video_.height = height;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (unsigned y = 0; y < height; ++y) {
        auto* destination = video_.pixels.data() + y * width;
        const auto* row = bytes + y * pitch;
        if (pixelFormat_ == retro::XRGB8888) {
            std::memcpy(destination, row, width * pixelSize);
            continue;
        }
        for (unsigned x = 0; x < width; ++x) {
            uint16_t pixel;
            std::memcpy(&pixel, row + x * pixelSize, sizeof(pixel));
            const bool rgb565 = pixelFormat_ == retro::RGB565;
            const auto red = expandColor((pixel >> (rgb565 ? 11 : 10)) & 31, 31);
            const auto green = expandColor((pixel >> 5) & (rgb565 ? 63 : 31), rgb565 ? 63 : 31);
            const auto blue = expandColor(pixel & 31, 31);
            destination[x] = (red << 16) | (green << 8) | blue;
        }
    }
}

void Core::checkCallbacks() const
{
    if (callbackFailed_) {
        throw std::runtime_error("The emulator could not process a callback from Gambatte.");
    }
}

bool Core::onEnvironment(unsigned command, void* data) noexcept
{
    try {
        return active_->environment(command, data);
    } catch (...) {
        active_->callbackFailed_ = true;
        return false;
    }
}

void Core::onVideo(const void* data, unsigned width, unsigned height, size_t pitch) noexcept
{
    try {
        active_->receiveVideo(data, width, height, pitch);
    } catch (...) {
        active_->callbackFailed_ = true;
    }
}

void Core::onAudio(int16_t left, int16_t right) noexcept
{
    const int16_t sample[]{left, right};
    onAudioBatch(sample, 1);
}

size_t Core::onAudioBatch(const int16_t* data, size_t frames) noexcept
{
    auto& audio = active_->audio_;
    if (data && frames <= (maxAudioSamples - audio.size()) / 2) {
        audio.insert(audio.end(), data, data + frames * 2);
        return frames;
    }
    return 0;
}

int16_t Core::onInputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port || device != retro::joypad || index) {
        return 0;
    }
    if (id == retro::joypadMask) {
        return static_cast<int16_t>(active_->buttons_);
    }
    return id < 16 && ((active_->buttons_ >> id) & 1);
}
