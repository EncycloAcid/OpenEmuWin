#include "Audio.hpp"

#include <algorithm>

Audio::~Audio()
{
    close();
}

bool Audio::configure(unsigned rate)
{
    if (rate_ == rate) {
        return device_ != nullptr;
    }

    close();
    rate_ = rate;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * (format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = rate * format.nBlockAlign;
    if (waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        device_ = nullptr;
        return false;
    }

    for (auto& buffer : buffers_) {
        buffer.header = {};
        buffer.header.lpData = reinterpret_cast<char*>(buffer.samples.data());
        buffer.header.dwBufferLength = sizeof(buffer.samples);
        if (waveOutPrepareHeader(device_, &buffer.header, sizeof(buffer.header)) != MMSYSERR_NOERROR) {
            close();
            return false;
        }
    }
    return true;
}

void Audio::submit(const std::vector<int16_t>& samples)
{
    if (!device_) {
        return;
    }

    for (size_t offset = 0; offset < samples.size();) {
        auto& buffer = buffers_[next_];
        if (buffer.header.dwFlags & WHDR_INQUEUE) {
            return;
        }

        const size_t count = std::min(samples.size() - offset, buffer.samples.size() - filled_);
        std::copy_n(samples.data() + offset, count, buffer.samples.data() + filled_);
        offset += count;
        filled_ += count;
        if (filled_ == buffer.samples.size()) {
            waveOutWrite(device_, &buffer.header, sizeof(buffer.header));
            next_ = (next_ + 1) % buffers_.size();
            filled_ = 0;
        }
    }
}

void Audio::clear()
{
    if (device_) {
        waveOutReset(device_);
    }
    next_ = 0;
    filled_ = 0;
}

void Audio::close()
{
    if (!device_) {
        return;
    }
    clear();
    for (auto& buffer : buffers_) {
        if (buffer.header.dwFlags & WHDR_PREPARED) {
            waveOutUnprepareHeader(device_, &buffer.header, sizeof(buffer.header));
        }
    }
    waveOutClose(device_);
    device_ = nullptr;
}
