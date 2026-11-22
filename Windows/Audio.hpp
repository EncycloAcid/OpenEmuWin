#pragma once

#include <windows.h>
#include <mmsystem.h>

#include <array>
#include <cstdint>
#include <vector>

class Audio {
public:
    Audio() = default;
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool configure(unsigned rate);
    void submit(const std::vector<int16_t>& samples);
    void clear();

private:
    struct Buffer {
        std::array<int16_t, 1024> samples{};
        WAVEHDR header{};
    };

    void close();

    HWAVEOUT device_ = nullptr;
    std::array<Buffer, 8> buffers_{};
    unsigned rate_ = 0;
    size_t next_ = 0;
    size_t filled_ = 0;
};
