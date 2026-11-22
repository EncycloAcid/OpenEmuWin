#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct Library {
    std::filesystem::path core;
    std::vector<std::filesystem::path> games;
};

std::filesystem::path appDataPath();
Library readLibrary(const std::filesystem::path& path);
void writeLibrary(const std::filesystem::path& path, const Library& library);
std::vector<uint8_t> readData(const std::filesystem::path& path);
void writeData(const std::filesystem::path& path, const std::vector<uint8_t>& data);
std::wstring hashRom(std::vector<uint8_t>& data);
