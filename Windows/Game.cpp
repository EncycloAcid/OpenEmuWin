#include "Game.hpp"
#include "Storage.hpp"

#include <xinput.h>

#include <algorithm>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
constexpr struct BatteryRegion {
    unsigned id;
    const wchar_t* extension;
} batteryRegions[]{
    {retro::saveRam, L".srm"},
    {retro::rtc, L".rtc"},
};

uint16_t controllerButtons()
{
    XINPUT_STATE state{};
    if (XInputGetState(0, &state) != ERROR_SUCCESS) {
        return 0;
    }

    const auto& gamepad = state.Gamepad;
    const auto has = [&](WORD button) { return (gamepad.wButtons & button) != 0; };
    constexpr SHORT deadzone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    uint16_t buttons = 0;
    if (has(XINPUT_GAMEPAD_A)) buttons |= retro::bit(retro::A);
    if (has(XINPUT_GAMEPAD_B)) buttons |= retro::bit(retro::B);
    if (has(XINPUT_GAMEPAD_START)) buttons |= retro::bit(retro::Start);
    if (has(XINPUT_GAMEPAD_BACK)) buttons |= retro::bit(retro::Select);
    if (has(XINPUT_GAMEPAD_DPAD_UP) || gamepad.sThumbLY > deadzone) buttons |= retro::bit(retro::Up);
    if (has(XINPUT_GAMEPAD_DPAD_DOWN) || gamepad.sThumbLY < -deadzone) buttons |= retro::bit(retro::Down);
    if (has(XINPUT_GAMEPAD_DPAD_LEFT) || gamepad.sThumbLX < -deadzone) buttons |= retro::bit(retro::Left);
    if (has(XINPUT_GAMEPAD_DPAD_RIGHT) || gamepad.sThumbLX > deadzone) buttons |= retro::bit(retro::Right);
    return buttons;
}
}

Game::Game(const fs::path& rom, const fs::path& core, const fs::path& appData)
    : core_(core), gameName_(rom.stem().wstring())
{
    const auto system = appData / L"System";
    const auto saves = appData / L"Saves";
    fs::create_directories(system);
    fs::create_directories(saves);

    auto data = readData(rom);
    if (data.size() < 32 * 1024) {
        throw std::runtime_error("Select a complete Game Boy or Game Boy Color ROM.");
    }
    saveBase_ = saves / hashRom(data);
    core_.load(rom, std::move(data), system, saves);
    for (const auto& region : batteryRegions) {
        const auto path = savePath(region.extension);
        if (fs::exists(path)) {
            core_.writeMemory(region.id, readData(path));
        }
    }

    soundAvailable_ = audio_.configure(static_cast<unsigned>(core_.av().timing.sampleRate + 0.5));
    timerResolutionChanged_ = timeBeginPeriod(1) == TIMERR_NOERROR;
}

Game::~Game()
{
    if (timerResolutionChanged_) {
        timeEndPeriod(1);
    }
}

void Game::frame(HWND window)
{
    const auto now = Clock::now();
    const uint16_t buttons = GetFocus() == window ? keyButtons_ | controllerButtons() : 0;
    core_.run(buttons);
    soundAvailable_ = audio_.configure(static_cast<unsigned>(core_.av().timing.sampleRate + 0.5));
    audio_.submit(core_.audio());
    InvalidateRect(window, nullptr, FALSE);

    const auto period = std::chrono::duration<double>(1.0 / core_.av().timing.fps);
    const auto step = std::chrono::duration_cast<Clock::duration>(period);
    nextFrame_ = std::max(nextFrame_ + step, now);
}

void Game::paint(HWND window) const
{
    PAINTSTRUCT paint;
    const auto dc = BeginPaint(window, &paint);
    FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    const auto& frame = core_.video();
    RECT client;
    GetClientRect(window, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width > 0 && height > 0 && frame.width && frame.height) {
        double aspect = core_.av().geometry.aspectRatio;
        if (aspect <= 0) {
            aspect = static_cast<double>(frame.width) / frame.height;
        }
        int drawWidth = width;
        int drawHeight = height;
        if (width > height * aspect) {
            drawWidth = std::max(1, static_cast<int>(height * aspect));
        } else {
            drawHeight = std::max(1, static_cast<int>(width / aspect));
        }

        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = frame.width;
        bitmap.bmiHeader.biHeight = -static_cast<LONG>(frame.height);
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, (width - drawWidth) / 2, (height - drawHeight) / 2, drawWidth, drawHeight,
                      0, 0, frame.width, frame.height, frame.pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
    }
    EndPaint(window, &paint);
}

void Game::key(unsigned key, bool down)
{
    uint16_t button;
    switch (key) {
    case 'X': button = retro::bit(retro::A); break;
    case 'Z': button = retro::bit(retro::B); break;
    case VK_RETURN: button = retro::bit(retro::Start); break;
    case VK_SHIFT: button = retro::bit(retro::Select); break;
    case VK_UP: button = retro::bit(retro::Up); break;
    case VK_DOWN: button = retro::bit(retro::Down); break;
    case VK_LEFT: button = retro::bit(retro::Left); break;
    case VK_RIGHT: button = retro::bit(retro::Right); break;
    default: return;
    }

    if (down) {
        keyButtons_ |= button;
    } else {
        keyButtons_ &= static_cast<uint16_t>(~button);
    }
}

fs::path Game::savePath(const wchar_t* extension) const
{
    auto path = saveBase_;
    path += extension;
    return path;
}

void Game::persistBattery() const
{
    for (const auto& region : batteryRegions) {
        const auto data = core_.readMemory(region.id);
        if (!data.empty()) {
            writeData(savePath(region.extension), data);
        }
    }
}

void Game::quickSave() const
{
    writeData(savePath(L".state"), core_.saveState());
}

void Game::quickLoad()
{
    const auto path = savePath(L".state");
    if (!fs::exists(path)) {
        throw std::runtime_error("There is no quick save for this game yet.");
    }
    core_.loadState(readData(path));
    audio_.clear();
}

void Game::reset()
{
    core_.reset();
    audio_.clear();
}

void Game::pause(bool paused)
{
    paused_ = paused;
    audio_.clear();
    nextFrame_ = Clock::now();
}
