#include "Game.hpp"
#include "Storage.hpp"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
constexpr wchar_t libraryClass[] = L"OpenEmuWin.Library";
constexpr wchar_t playerClass[] = L"OpenEmuWin.Player";

enum Command : unsigned {
    AddGames = 100, Play, Remove, SelectCore,
    CloseGame, PauseGame, ResetGame, QuickSave, QuickLoad,
};

std::wstring wide(const char* text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (size <= 0) {
        return L"An unexpected error occurred.";
    }
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), size);
    result.pop_back();
    return result;
}

void showError(HWND window, const std::exception& error)
{
    MessageBoxW(window, wide(error.what()).c_str(), L"OpenEmuWin", MB_OK | MB_ICONERROR);
}

std::wstring extension(const fs::path& path)
{
    auto value = path.extension().wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return std::towlower(ch); });
    return value;
}

bool samePath(const fs::path& left, const fs::path& right)
{
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::vector<fs::path> chooseFiles(HWND owner, const wchar_t* title, const wchar_t* filter, bool multiple)
{
    std::array<wchar_t, 32768> buffer{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrTitle = title;
    dialog.lpstrFilter = filter;
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (multiple) {
        dialog.Flags |= OFN_ALLOWMULTISELECT;
    }
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError() != 0) {
            throw std::runtime_error("Could not read the file selection. Try selecting fewer files.");
        }
        return {};
    }

    fs::path first(buffer.data());
    const auto* name = buffer.data() + wcslen(buffer.data()) + 1;
    if (!*name) {
        return {std::move(first)};
    }

    std::vector<fs::path> result;
    while (*name) {
        result.push_back(first / name);
        name += wcslen(name) + 1;
    }
    return result;
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}
    ~App()
    {
        if (profileLock_ != INVALID_HANDLE_VALUE) {
            CloseHandle(profileLock_);
        }
    }

    int run(int show)
    {
        appData_ = appDataPath();
        fs::create_directories(appData_);
        profileLock_ = CreateFileW((appData_ / L"Library.lock").c_str(), GENERIC_WRITE, 0,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (profileLock_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(GetLastError() == ERROR_SHARING_VIOLATION
                ? "OpenEmuWin is already running."
                : "Could not open the OpenEmuWin data folder.");
        }
        library_ = readLibrary(libraryPath());

        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        if (!InitCommonControlsEx(&controls)) {
            throw std::runtime_error("Could not initialize Windows Common Controls.");
        }
        registerClass(libraryClass, libraryProc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        registerClass(playerClass, playerProc, nullptr);
        libraryWindow_ = CreateWindowExW(0, libraryClass, L"OpenEmuWin",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
            770, 480, nullptr, nullptr, instance_, this);
        if (!libraryWindow_) {
            throw std::runtime_error("Could not create the library window.");
        }
        refreshLibrary();
        ShowWindow(libraryWindow_, show);

        for (;;) {
            MSG message;
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    return static_cast<int>(message.wParam);
                }
                if (game_ || !IsDialogMessageW(libraryWindow_, &message)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            } else {
                DWORD timeout = INFINITE;
                if (game_ && !game_->paused()) {
                    const auto delay = std::chrono::ceil<std::chrono::milliseconds>(
                        game_->deadline() - Game::Clock::now());
                    timeout = static_cast<DWORD>(std::max<int64_t>(0, delay.count()));
                }
                MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }

            if (game_ && !game_->paused() && Game::Clock::now() >= game_->deadline()) {
                try {
                    game_->frame(playerWindow_);
                    if (game_->shutdownRequested()) {
                        closeGame();
                    }
                } catch (const std::exception& error) {
                    game_->pause(true);
                    updatePlayerTitle();
                    showError(playerWindow_, error);
                }
            }
        }
    }

private:
    fs::path libraryPath() const { return appData_ / L"Library.txt"; }

    void registerClass(const wchar_t* name, WNDPROC proc, HBRUSH background) const
    {
        WNDCLASSW window{};
        window.hInstance = instance_;
        window.lpszClassName = name;
        window.lpfnWndProc = proc;
        window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        window.hbrBackground = background;
        if (!RegisterClassW(&window)) {
            throw std::runtime_error("Could not register the OpenEmuWin window.");
        }
    }

    HWND makeControl(HWND owner, const wchar_t* type, const wchar_t* title, DWORD style,
                     unsigned id = 0, DWORD exStyle = 0) const
    {
        auto window = CreateWindowExW(exStyle, type, title, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, owner, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance_, nullptr);
        if (!window) {
            throw std::runtime_error("Could not create a library control.");
        }
        SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        return window;
    }

    void createLibraryControls(HWND window)
    {
        add_ = makeControl(window, L"BUTTON", L"Add Games...", WS_TABSTOP, AddGames);
        play_ = makeControl(window, L"BUTTON", L"Play", WS_TABSTOP, Play);
        remove_ = makeControl(window, L"BUTTON", L"Remove", WS_TABSTOP, Remove);
        selectCore_ = makeControl(window, L"BUTTON", L"Select Gambatte Core...", WS_TABSTOP, SelectCore);
        list_ = makeControl(window, WC_LISTVIEWW, L"ROM Library",
                            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                            0, WS_EX_CLIENTEDGE);
        status_ = makeControl(window, L"STATIC", L"", SS_ENDELLIPSIS);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        constexpr const wchar_t* headers[]{L"Game", L"System", L"ROM File"};
        for (int index = 0; index < 3; ++index) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT;
            column.pszText = const_cast<wchar_t*>(headers[index]);
            if (ListView_InsertColumn(list_, index, &column) < 0) {
                throw std::runtime_error("Could not create the ROM library columns.");
            }
        }
    }

    void resizeLibrary(int width, int height) const
    {
        MoveWindow(add_, 12, 12, 98, 30, TRUE);
        MoveWindow(play_, 118, 12, 68, 30, TRUE);
        MoveWindow(remove_, 194, 12, 78, 30, TRUE);
        MoveWindow(selectCore_, width - 176, 12, 164, 30, TRUE);
        MoveWindow(list_, 12, 54, width - 24, height - 88, TRUE);
        MoveWindow(status_, 12, height - 24, width - 24, 18, TRUE);
        const int titleWidth = std::max(130, (width - 24) / 3);
        ListView_SetColumnWidth(list_, 0, titleWidth);
        ListView_SetColumnWidth(list_, 1, 140);
        ListView_SetColumnWidth(list_, 2, std::max(70, width - 50 - titleWidth - 140));
    }

    void refreshLibrary() const
    {
        ListView_DeleteAllItems(list_);
        for (int index = 0; index < static_cast<int>(library_.games.size()); ++index) {
            const auto& path = library_.games[index];
            auto name = path.stem().wstring();
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.pszText = name.data();
            if (ListView_InsertItem(list_, &item) != index) {
                throw std::runtime_error("Could not display the ROM library.");
            }
            std::wstring system = extension(path) == L".gbc" ? L"Game Boy Color" : L"Game Boy";
            auto file = path.wstring();
            ListView_SetItemText(list_, index, 1, system.data());
            ListView_SetItemText(list_, index, 2, file.data());
        }

        updateLibraryStatus();
        updateSelection();
    }

    void updateLibraryStatus() const
    {
        std::wstring status = library_.core.empty()
            ? L"Select a Windows x64 Gambatte core to play."
            : L"Core: " + library_.core.filename().wstring();
        if (library_.games.empty()) {
            status = L"Add .gb or .gbc games to your library.  " + status;
        }
        SetWindowTextW(status_, status.c_str());
    }

    int selection() const { return ListView_GetNextItem(list_, -1, LVNI_SELECTED); }

    void updateSelection() const
    {
        const bool selected = selection() >= 0;
        EnableWindow(play_, selected);
        EnableWindow(remove_, selected);
    }

    void saveLibrary(Library next)
    {
        writeLibrary(libraryPath(), next);
        library_ = std::move(next);
        refreshLibrary();
    }

    void addGames()
    {
        const auto files = chooseFiles(libraryWindow_, L"Add games",
            L"Game Boy ROMs (*.gb;*.gbc)\0*.gb;*.gbc\0", true);
        if (files.empty()) {
            return;
        }

        auto next = library_;
        for (const auto& file : files) {
            if (extension(file) != L".gb" && extension(file) != L".gbc") {
                throw std::runtime_error("Select Game Boy (.gb) or Game Boy Color (.gbc) ROMs.");
            }
            const auto found = std::find_if(next.games.begin(), next.games.end(),
                [&](const fs::path& game) { return samePath(game, file); });
            if (found == next.games.end()) {
                next.games.push_back(file);
            }
        }
        saveLibrary(std::move(next));
        const auto selected = std::find_if(library_.games.begin(), library_.games.end(),
            [&](const fs::path& game) { return samePath(game, files.front()); });
        const auto index = static_cast<int>(selected - library_.games.begin());
        ListView_SetItemState(list_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, index, FALSE);
        SetFocus(list_);
    }

    bool selectCore()
    {
        const auto files = chooseFiles(libraryWindow_, L"Select a Windows x64 Gambatte libretro core",
            L"Libretro cores (*.dll)\0*.dll\0", false);
        if (files.empty()) {
            return false;
        }

        Core check(files.front());
        auto next = library_;
        next.core = files.front();
        writeLibrary(libraryPath(), next);
        library_.core = std::move(next.core);
        updateLibraryStatus();
        return true;
    }

    void playGame()
    {
        const int index = selection();
        if (index < 0 || game_) {
            return;
        }
        if ((library_.core.empty() || !fs::is_regular_file(library_.core)) && !selectCore()) {
            return;
        }

        game_ = std::make_unique<Game>(library_.games.at(index), library_.core, appData_);
        playerWindow_ = CreateWindowExW(0, playerClass, L"OpenEmuWin", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 664, 640, libraryWindow_, nullptr, instance_, this);
        if (!playerWindow_) {
            game_.reset();
            throw std::runtime_error("Could not create the game window.");
        }
        updatePlayerTitle();
        EnableWindow(libraryWindow_, FALSE);
        ShowWindow(playerWindow_, SW_SHOW);
        SetFocus(playerWindow_);
    }

    void closeGame()
    {
        try {
            game_->persistBattery();
        } catch (const std::exception& error) {
            const auto text = wide(error.what()) + L"\n\nClose the game without saving?";
            if (MessageBoxW(playerWindow_, text.c_str(), L"OpenEmuWin",
                            MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
                game_->pause(true);
                updatePlayerTitle();
                return;
            }
        }
        EnableWindow(libraryWindow_, TRUE);
        DestroyWindow(playerWindow_);
        playerWindow_ = nullptr;
        game_.reset();
        SetActiveWindow(libraryWindow_);
        SetFocus(list_);
    }

    void updatePlayerTitle(const wchar_t* action = nullptr) const
    {
        auto title = game_->name() + L" - OpenEmuWin";
        if (game_->paused()) title += L" (Paused)";
        if (!game_->soundAvailable()) title += L" (Audio unavailable)";
        if (action) title += std::wstring(L" - ") + action;
        SetWindowTextW(playerWindow_, title.c_str());
        CheckMenuItem(GetMenu(playerWindow_), PauseGame, MF_BYCOMMAND | (game_->paused() ? MF_CHECKED : MF_UNCHECKED));
    }

    void gameCommand(unsigned command)
    {
        switch (command) {
        case CloseGame: closeGame(); break;
        case PauseGame:
            game_->pause(!game_->paused());
            updatePlayerTitle();
            break;
        case ResetGame:
            game_->reset();
            updatePlayerTitle(L"Reset");
            break;
        case QuickSave:
            game_->quickSave();
            updatePlayerTitle(L"Quick save written");
            break;
        case QuickLoad:
            game_->quickLoad();
            updatePlayerTitle(L"Quick save loaded");
            break;
        }
    }

    LRESULT libraryMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message) {
        case WM_CREATE:
            createLibraryControls(window);
            return 0;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) resizeLibrary(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = {520, 300};
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case AddGames: addGames(); break;
            case Play: playGame(); break;
            case SelectCore: selectCore(); break;
            case Remove: {
                const int index = selection();
                if (index >= 0) {
                    auto next = library_;
                    next.games.erase(next.games.begin() + index);
                    saveLibrary(std::move(next));
                }
                break;
            }
            }
            return 0;
        case WM_NOTIFY: {
            const auto* notification = reinterpret_cast<const NMHDR*>(lParam);
            if (notification->hwndFrom == list_) {
                if (notification->code == LVN_ITEMCHANGED) updateSelection();
                if (notification->code == NM_DBLCLK || notification->code == NM_RETURN) playGame();
            }
            return 0;
        }
        case WM_CLOSE:
            if (game_) closeGame();
            if (!game_) DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    LRESULT playerMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message) {
        case WM_CREATE: {
            auto menu = CreateMenu();
            auto commands = CreatePopupMenu();
            AppendMenuW(commands, MF_STRING, PauseGame, L"&Pause\tP");
            AppendMenuW(commands, MF_STRING, ResetGame, L"&Reset\tF2");
            AppendMenuW(commands, MF_STRING, QuickSave, L"Quick &Save\tF5");
            AppendMenuW(commands, MF_STRING, QuickLoad, L"Quick &Load\tF8");
            AppendMenuW(commands, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(commands, MF_STRING, CloseGame, L"&Close\tEsc");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(commands), L"&Game");
            SetMenu(window, menu);
            return 0;
        }
        case WM_KEYDOWN:
            if (!(lParam & (1LL << 30))) {
                switch (wParam) {
                case VK_ESCAPE: gameCommand(CloseGame); return 0;
                case 'P': gameCommand(PauseGame); return 0;
                case VK_F2: gameCommand(ResetGame); return 0;
                case VK_F5: gameCommand(QuickSave); return 0;
                case VK_F8: gameCommand(QuickLoad); return 0;
                }
            }
            game_->key(static_cast<unsigned>(wParam), true);
            return 0;
        case WM_KEYUP:
            game_->key(static_cast<unsigned>(wParam), false);
            return 0;
        case WM_KILLFOCUS:
        case WM_ENTERMENULOOP:
            game_->clearInput();
            return 0;
        case WM_COMMAND:
            gameCommand(LOWORD(wParam));
            return 0;
        case WM_PAINT:
            game_->paint(window);
            return 0;
        case WM_SIZE:
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            closeGame();
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    template <auto handler>
    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!app) {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        try {
            return (app->*handler)(window, message, wParam, lParam);
        } catch (const std::exception& error) {
            showError(window, error);
            return message == WM_CREATE ? -1 : 0;
        }
    }

    static LRESULT CALLBACK libraryProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        return dispatch<&App::libraryMessage>(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK playerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        return dispatch<&App::playerMessage>(window, message, wParam, lParam);
    }

    HINSTANCE instance_;
    HANDLE profileLock_ = INVALID_HANDLE_VALUE;
    HWND libraryWindow_ = nullptr;
    HWND playerWindow_ = nullptr;
    HWND add_ = nullptr;
    HWND play_ = nullptr;
    HWND remove_ = nullptr;
    HWND selectCore_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    fs::path appData_;
    Library library_;
    std::unique_ptr<Game> game_;
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    try {
        return App(instance).run(show);
    } catch (const std::exception& error) {
        showError(nullptr, error);
        return 1;
    }
}
