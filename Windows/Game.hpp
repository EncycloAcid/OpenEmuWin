#pragma once

#include "Audio.hpp"
#include "Core.hpp"

#include <chrono>

class Game {
public:
    using Clock = std::chrono::steady_clock;

    Game(const std::filesystem::path& rom, const std::filesystem::path& core,
         const std::filesystem::path& appData);
    ~Game();

    void frame(HWND window);
    void paint(HWND window) const;
    void key(unsigned key, bool down);
    void clearInput() { keyButtons_ = 0; }

    void persistBattery() const;
    void quickSave() const;
    void quickLoad();
    void reset();
    void pause(bool paused);

    bool paused() const { return paused_; }
    bool soundAvailable() const { return soundAvailable_; }
    bool shutdownRequested() const { return core_.shutdownRequested(); }
    Clock::time_point deadline() const { return nextFrame_; }
    const std::wstring& name() const { return gameName_; }

private:
    std::filesystem::path savePath(const wchar_t* extension) const;

    Core core_;
    Audio audio_;
    std::wstring gameName_;
    std::filesystem::path saveBase_;
    bool paused_ = false;
    bool soundAvailable_ = false;
    bool timerResolutionChanged_ = false;
    uint16_t keyButtons_ = 0;
    Clock::time_point nextFrame_ = Clock::now();
};
