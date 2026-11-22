# OpenEmuWin for Windows

OpenEmuWin is a Windows x64 frontend for Game Boy and Game Boy Color games. It uses a local ROM library and the Gambatte libretro core. It supports Windows 10 version 1903 or later and Windows 11.

## Play a game

1. Extract the OpenEmuWin ZIP into a folder.
2. Download the Windows x64 Gambatte archive and extract it into a folder you can keep. [Gambatte for Windows x64](https://buildbot.libretro.com/nightly/windows/x86_64/latest/gambatte_libretro.dll.zip)
3. Run `OpenEmuWin.exe`, click **Select Gambatte Core**, and select `gambatte_libretro.dll`.
4. Click **Add Games** and select your `.gb` or `.gbc` files. Double-click a game or select it and click **Play**.

The library stores links to your ROM files, so keep the files in place. **Remove** drops a game from the list and leaves the ROM and saves on disk. Battery saves, real-time clock data, and each game's quick save are stored under `%LOCALAPPDATA%\OpenEmuWin\Saves`. Save filenames use the ROM's SHA-256 hash, so re-adding a moved ROM retains its saves.

| Action | Keyboard | XInput controller |
| --- | --- | --- |
| Move | Arrow keys | D-pad or left stick |
| A / B | X / Z | A / B |
| Start / Select | Enter / Shift | Menu / View |
| Pause or resume | P | |
| Reset | F2 | |
| Write or load a quick save | F5 / F8 | |
| Return to library | Esc | |

This release supports one player and uncompressed ROMs. It does not import macOS libraries, core bundles, shaders, or save states. A quick save should be loaded with the same Gambatte build used to create it.

## Build on Windows

From a terminal with CMake 3.20 or later and Visual Studio 2022 with the Desktop development with C++ workload, run the following commands from the repository root.

```powershell
cmake -S Windows -B build-windows -G "Visual Studio 17 2022" -A x64
cmake --build build-windows --config Release
cmake --install build-windows --config Release --prefix dist
```

The `dist` directory will contain the executable and this guide. Gambatte is a separate dependency downloaded by the user.

## How the original app maps to Windows

| OpenEmu source | Windows implementation |
| --- | --- |
| `OpenEmu/ImportOperation.swift` identifies and organizes ROMs. `OpenEmu/OELibraryDatabase.swift` stores the library in Core Data. | `Storage.cpp` persists a list of paths to existing ROMs. The Win32 library window in `App.cpp` handles adding, removing, and launching games. |
| `OpenEmu/OEGameDocument.swift` selects an `OECorePlugin` and launches emulation through a thread manager or an XPC manager and `OpenEmuHelperApp`. | `Core.cpp` implements the libretro API callbacks used by Gambatte and loads the Windows DLL in the app process. |
| `OpenEmu/GameViewController.swift` and OpenEmuKit present game frames in the Cocoa frontend. | `Game.cpp` presents pixel frames with Win32 GDI, polls XInput, and manages timing and saves. `Audio.cpp` queues stereo PCM through WinMM. |
| `OpenEmu/CoreUpdater.swift` discovers macOS core plugins through appcasts. | The Windows app remembers the path to the Gambatte DLL selected in the library window. |

The core smoke test uses generated Game Boy and Game Boy Color test cartridges to exercise frame delivery, audio, input, battery RAM, and save-state round trips. To run it locally, configure CMake with `-DGAMBATTE_SMOKE_CORE=<absolute path to a Gambatte shared library>`, build, and run CTest. Use a DLL with a Windows build or a shared object with a Linux build.
