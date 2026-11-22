#pragma once

#include <cstddef>
#include <cstdint>

// The subset of libretro API v1 used by the Gambatte host.
// Reference: https://github.com/libretro/libretro-common/blob/ae22debd425c33fe355ac2e51d9a140d7e8b3fa6/include/libretro.h
namespace retro {
constexpr unsigned apiVersion = 1;
constexpr unsigned joypad = 1;
constexpr unsigned joypadMask = 256;
constexpr unsigned saveRam = 0;
constexpr unsigned rtc = 1;
constexpr unsigned systemRam = 2;

enum Button : unsigned { B = 0, Select = 2, Start, Up, Down, Left, Right, A };
constexpr uint16_t bit(Button button) { return uint16_t(1u << button); }

enum Environment : unsigned {
    GetCanDupe = 3,
    Shutdown = 7,
    SystemDirectory = 9,
    SetPixelFormat = 10,
    SetInputDescriptors = 11,
    GetVariable = 15,
    SetVariables = 16,
    GetVariableUpdate = 17,
    GetSaveDirectory = 31,
    SetSystemAVInfo = 32,
    SetGeometry = 37,
    GetLanguage = 39,
    GetInputBitmasks = 51 | 0x10000,
    GetCoreOptionsVersion = 52,
};

enum PixelFormat : int { RGB1555 = 0, XRGB8888, RGB565 };

struct SystemInfo {
    const char* libraryName;
    const char* libraryVersion;
    const char* validExtensions;
    bool needFullpath;
    bool blockExtract;
};

struct GameInfo {
    const char* path;
    const void* data;
    size_t size;
    const char* metadata;
};

struct Geometry {
    unsigned baseWidth, baseHeight, maxWidth, maxHeight;
    float aspectRatio;
};

struct Timing {
    double fps, sampleRate;
};

struct AVInfo {
    Geometry geometry;
    Timing timing;
};

struct Variable {
    const char* key;
    const char* value;
};

using EnvironmentCallback = bool (*)(unsigned, void*);
using VideoCallback = void (*)(const void*, unsigned, unsigned, size_t);
using AudioCallback = void (*)(int16_t, int16_t);
using AudioBatchCallback = size_t (*)(const int16_t*, size_t);
using InputPollCallback = void (*)();
using InputStateCallback = int16_t (*)(unsigned, unsigned, unsigned, unsigned);
}
