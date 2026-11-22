#include "Storage.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
void replaceFile(const fs::path& path, const char* data, size_t size)
{
    auto temporary = path;
    temporary += L".tmp";
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(data, size);
        stream.close();
        if (!stream || !MoveFileExW(temporary.c_str(), path.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Could not write to the OpenEmuWin data folder.");
        }
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}
}

fs::path appDataPath()
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        throw std::runtime_error("Could not locate the Windows Local AppData folder.");
    }
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> ownedPath(path, CoTaskMemFree);
    return fs::path(ownedPath.get()) / L"OpenEmuWin";
}

Library readLibrary(const fs::path& path)
{
    if (!fs::exists(path)) {
        return {};
    }

    std::ifstream stream(path, std::ios::binary);
    Library library;
    std::string version, value;
    if (!std::getline(stream, version) || version != "OpenEmuWin 1" ||
        !(stream >> std::quoted(value))) {
        throw std::runtime_error("Could not read the OpenEmuWin library.");
    }
    library.core = fs::u8path(value);
    while (stream >> std::ws && !stream.eof()) {
        if (!(stream >> std::quoted(value)) || value.empty()) {
            throw std::runtime_error("Could not read the OpenEmuWin library.");
        }
        library.games.push_back(fs::u8path(value));
    }
    if (stream.bad()) {
        throw std::runtime_error("Could not read the OpenEmuWin library.");
    }
    return library;
}

void writeLibrary(const fs::path& path, const Library& library)
{
    std::ostringstream stream;
    stream << "OpenEmuWin 1\n" << std::quoted(library.core.u8string()) << '\n';
    for (const auto& game : library.games) {
        stream << std::quoted(game.u8string()) << '\n';
    }
    const auto text = stream.str();
    replaceFile(path, text.data(), text.size());
}

std::vector<uint8_t> readData(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto length = stream.tellg();
    if (length < 0 || length > 32 * 1024 * 1024) {
        throw std::runtime_error("Could not read the file. Check that it exists and is 32 MB or smaller.");
    }
    std::vector<uint8_t> data(static_cast<size_t>(length));
    stream.seekg(0);
    if (!data.empty() && !stream.read(reinterpret_cast<char*>(data.data()), length)) {
        throw std::runtime_error("Could not read the entire file.");
    }
    return data;
}

void writeData(const fs::path& path, const std::vector<uint8_t>& data)
{
    replaceFile(path, reinterpret_cast<const char*>(data.data()), data.size());
}

std::wstring hashRom(std::vector<uint8_t>& data)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("Could not open the Windows SHA-256 provider.");
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<uint8_t, 32> digest{};
    const bool success = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
                         BCryptHashData(hash, data.data(), static_cast<ULONG>(data.size()), 0) >= 0 &&
                         BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!success) {
        throw std::runtime_error("Could not calculate the ROM hash.");
    }

    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring name;
    for (const auto byte : digest) {
        name += digits[byte >> 4];
        name += digits[byte & 15];
    }
    return name;
}
