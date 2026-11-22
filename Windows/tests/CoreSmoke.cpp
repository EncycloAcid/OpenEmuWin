#include "Core.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
std::vector<uint8_t> makeRom(bool color)
{
    // Test cartridge. It enables its battery-backed RAM, selects the button
    // keys in P1 and mirrors the active-low A button into the first RAM byte.
    constexpr uint8_t entry[]{0x00, 0xc3, 0x50, 0x01};
    constexpr uint8_t program[]{
        0xf3, 0x31, 0xfe, 0xff, 0x3e, 0x0a, 0xea, 0x00, 0x00, 0x3e, 0x10, 0xe0, 0x00,
        0xf0, 0x00, 0xe6, 0x01, 0xea, 0x00, 0xa0, 0x18, 0xf7,
    };
    constexpr char title[] = "OEMU TEST";
    std::vector<uint8_t> rom(32 * 1024);
    std::copy(std::begin(entry), std::end(entry), rom.begin() + 0x100);
    std::copy(std::begin(title), std::end(title) - 1, rom.begin() + 0x134);
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x150);
    rom[0x143] = color ? 0x80 : 0x00;
    rom[0x147] = 0x03; // MBC1 + RAM + battery
    rom[0x149] = 0x02; // 8 KiB of RAM
    for (size_t offset = 0x134; offset <= 0x14c; ++offset) {
        rom[0x14d] = static_cast<uint8_t>(rom[0x14d] - rom[offset] - 1);
    }
    return rom;
}

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int smoke(const fs::path& corePath)
{
    try {
        for (const bool color : {false, true}) {
            Core core(corePath);
            const auto directory = fs::current_path();
            core.load(color ? "test.gbc" : "test.gb", makeRom(color), directory, directory);
            require(core.av().timing.fps > 55 && core.av().timing.fps < 65, "Invalid frame rate.");

            for (int i = 0; i < 5; ++i) {
                core.run(0);
            }

            const auto& video = core.video();
            require(video.width == 160 && video.height == 144 && video.pixels.size() == 160 * 144,
                    "Expected a 160 x 144 frame.");
            require(!core.audio().empty() && core.audio().size() % 2 == 0, "Expected stereo audio.");

            auto ram = core.readMemory(retro::saveRam);
            require(ram.size() == 8192 && ram[0] == 1, "Expected released A button in battery RAM.");
            for (int i = 0; i < 5; ++i) {
                core.run(retro::bit(retro::A));
            }
            require(core.readMemory(retro::saveRam)[0] == 0, "The A button did not reach the cart.");
            ram[1] = 0x7b;
            core.writeMemory(retro::saveRam, ram);
            require(core.readMemory(retro::saveRam) == ram, "Battery data did not round-trip.");

            auto workingRam = core.readMemory(retro::systemRam);
            require(!workingRam.empty(), "Expected Game Boy working RAM.");
            workingRam[0] = 0x3c;
            core.writeMemory(retro::systemRam, workingRam);
            const auto state = core.saveState();
            workingRam[0] = 0xa7;
            core.writeMemory(retro::systemRam, workingRam);
            core.run(0);
            core.reset();
            core.loadState(state);
            require(core.readMemory(retro::systemRam)[0] == 0x3c, "Save state did not restore working RAM.");
        }
        std::cout << "Game Boy and Game Boy Color video, audio, input, battery RAM, and save states passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
}

#ifdef _WIN32
int wmain(int count, wchar_t** args)
#else
int main(int count, char** args)
#endif
{
    return count == 2 ? smoke(args[1]) : 2;
}
