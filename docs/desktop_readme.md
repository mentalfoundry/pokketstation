# Desktop frontend

SDL2-based standalone app for Windows.

## Usage

Prebuilt releases ship `pokketstation.exe` directly. Building from source instead places it at `build\Debug\pokketstation.exe` (the exact path depends on your `--config`; see [Building](#building) below). Either way, run:
```
pokketstation.exe [--console|--no-console] <bios.bin> <app-or-card-file>
```
Example, using a real BIOS dump and a memory-card image. Substitute your own paths; neither file is bundled. **No PocketStation BIOS is bundled.** The BIOS is copyrighted Sony firmware; you must supply your own dump, extracted from real hardware:
```
pokketstation.exe .\bios.bin .\samplememcard.mcr
```
- **The second file's extension does not matter.** This app picks the loader by content:
  - If the file's size exactly matches the real flash size, it loads as a full memory-card image (`.mcr`). Navigate its real BIOS menu with the keyboard, the same as real hardware.
  - Otherwise, it first tries the file as a single-save file with a real PS1 directory frame in front of it (`.mcs`). This is the format most PS1 save managers use to export a single save, and by far the more common of the two formats in practice.
  - Failing that, it tries the file as a bare raw PSX Title Sector app dump (`.pss`).

  Both `.mcr` card images and single-app loads are confirmed working end-to-end against a real BIOS and real extracted app dumps. See [hardware-notes.md](hardware-notes.md).
- **Double-clicking the .exe directly** (no command line) reuses the last BIOS path remembered from a previous run, if one exists (see `settings.cfg` below). Otherwise it falls back to `bios.bin` next to the executable. The app/card side always looks for `memcard.mcr` next to the executable; this path is never remembered.
- Launching with no BIOS and/or no app/card present, or an invalid one, is not fatal. The window still opens. Use **File > Load BIOS...** or **File > Open App/Card...** to browse to one instead.
- By default, the app runs without a console window, so its diagnostic `stderr` output has nowhere to go. Pass `--console` to get a console window, or `--no-console` to suppress it explicitly. Either flag is remembered in `settings.cfg` for future launches.
- **Controls:** arrow keys for Up/Down/Left/Right, **Z** for the Fire/Action button by default, and **F12** to write a diagnostic report. Remap any of these six from **Tools > Remap Controls...**. Picking a key already used by another row unbinds it from that row; two actions cannot share a key. An unbound row shows "(unbound)" and does nothing until remapped. The window is freely resizable; **View > Native Size (1x)** and **Double Size (2x)** are shortcuts back to a known-good size.
- **View > Colors** switches the LCD's rendered look: **Classic** (the default muted LCD-style ink-on-sage look), **Light** (black on white), **Dark** (white on black), or **Custom Colors...** (pick any pixel/background hex pair). **View > Sprite Shadows** toggles a faint one-row "ghosting" trail, approximating a real passive-matrix LCD's slow pixel response. This trail has its own configurable color.
- Press **F12** at any time to write a diagnostic report to a log file. See [Diagnostic reports](#diagnostic-reports-for-bug-reports) below.
- The PocketStation's hardware ID (`F_SN`, which sets Final Fantasy VIII Chocobo World's rank) defaults to the best rank. View or edit it from **Tools > Edit Hardware ID...**; see [hardware-notes.md](hardware-notes.md) for the format. This setting, and every other setting above (remembered BIOS path, hardware ID, color scheme, sprite-shadow state, key bindings, `--console`/`--no-console` preference), persists across relaunches in `settings.cfg` next to the executable. Each setting is written the moment it changes.
- **Help > About pokketstation...** shows the running version and a link back to this repo.
- **IR Link** connects two running copies of this app over IR, the same way two physical PocketStations would be held up to each other for local multiplayer. See [IR Link](#ir-link) below.

Single-app loads (`.pss`/`.mcs`) boot through the real BIOS menu the same way a full memory card does. See [Reaching a single loaded app](../README.md#reaching-a-single-loaded-app) in the main README for the button sequence.

## IR Link

Two separate running instances of `pokketstation.exe`, **on the same Windows machine**, can exchange real IR signals with each other. Each instance is a completely independent emulator, with its own window, its own loaded BIOS/app, and its own save state. IR is the only link between them, the same way two real PocketStation units work.

**To connect two instances:**
1. Launch `pokketstation.exe` twice (for example, run it once, then double-click it again, or launch a second copy from a terminal). You now have two separate windows.
2. In one window: **IR Link > Host Session**. The title bar shows "IR - Waiting...".
3. In the other window: **IR Link > Connect**. The title bar shows "IR - Connecting..." until the two find each other (usually instant), then both title bars show "IR - Connected".
4. Use each instance normally from there. Whatever the loaded app does with its IR port now reaches the other instance.
5. **IR Link > Disconnect** ends the session from either side at any time.

**Things to know:**
- This version does not prompt for a pipe or session name. Host and Connect always use the same well-known local connection. Only one linked pair can be active on a machine at a time.
- This only works between two instances on **one machine**. There is no network/remote play support.
- Loading a different BIOS or app/card, pressing **Reset**, and using **Load State** each drop an active IR Link automatically. All three reset the emulator's own IR state, so a link left connected across one would fall out of sync with the other instance. Reconnect through **IR Link > Connect** or **Host Session** afterward if you still need it.
- IR timing is inferred, not confirmed against real hardware. Two details in particular are inferred: how strongly this emulator filters a noisy signal, and what a receiving app reads back during a transfer. No app in this project's own test corpus has ever been traced using IR. An app that behaves differently over IR Link than on real hardware is worth reporting. See [hardware-notes.md](hardware-notes.md#ir--ir-link) for the technical detail.

## Building

### Windows

**Prerequisites:**
- Visual Studio (or the standalone [Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio)), with the **"Desktop development with C++"** workload. This workload includes the MSVC compiler and a bundled copy of CMake/Ninja; no separate CMake install is needed.
- [vcpkg](https://github.com/microsoft/vcpkg), needed for this frontend's SDL2 dependency (see below). Not needed if you only want to build the core, tools, tests, or the libretro core.

**1. Open the right terminal.** Regular `cmd`, PowerShell, and Git Bash windows do not have the compiler or CMake on `PATH`. From the Start Menu, search for and open **"Developer Command Prompt for VS"** (or "x64 Native Tools Command Prompt for VS"). This shortcut sets up the compiler environment for you automatically. If you cannot find that shortcut, open a normal `cmd` window and run this first to set it up manually. Adjust the version folder if yours differs from `18`:
```
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%
```

**2. `cd` to the repo.** If the repo is on a different drive than your terminal's current drive, plain `cd` does not switch drives. Use `/d`:
```
cd /d D:\path\to\pokketstation
```

**3. Configure and build.** CMake's Visual Studio generator is "multi-config": Debug and Release builds live side by side. Pass `--config` consistently to the build and test steps:
```
cmake -B build -S .
cmake --build build --config Debug
ctest --test-dir build -C Debug
```
This builds the core, tools, tests, and the libretro core. It skips this desktop frontend, with a warning, unless SDL2 is also available (see below).

### Linux / macOS

```
cmake -B build -S .
cmake --build build
ctest --test-dir build
```

### Enabling this frontend (SDL2)

This frontend is skipped, with a warning, if SDL2 is not found. The other targets (core, tools, tests) still build fine without it. Install SDL2 via vcpkg:
```
vcpkg install sdl2:x64-windows
```
Then **delete your existing `build/` folder and reconfigure from scratch**, pointing CMake at vcpkg's toolchain file. This step matters: CMake caches a "not found" result for SDL2. Reconfiguring in place, on top of an existing `build/` directory, will still fail after installing SDL2:
```
rmdir /s /q build
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Debug
```

## Diagnostic reports (for bug reports)

The desktop app can write a detailed diagnostic report to a log file, in two situations:

- **Automatically**, the first time the CPU hits an unrecognized or unimplemented opcode. This is always a real emulator bug, not something you did wrong. The emulator does not crash. It stops stepping the CPU, freezes on the last good frame, and prints a message pointing you at the report. This message is visible only if you launched with `--console` (see above).
- **On demand, at any time**, by pressing **F12**. Use this for anything that does not trigger a hard fault but still looks wrong: glitched graphics, missing sound, an app that seems stuck, and similar issues.

Each report is written to the current directory as `pokketstation_report_<timestamp>.log` (example: `pokketstation_report_20260721_143012.log`). Each report contains:
- Why it was written (fault or manual F12), and the frame number.
- The BIOS and app/card file paths you ran with.
- Total instructions executed, held button state, `CLK_MODE`, and flash bank-select state (`F_BANK_FLG`/`F_BANK_VAL`).
- All CPU registers, `PC`, and `CPSR`, including ARM vs. Thumb mode.
- If a fault occurred: the exact unrecognized opcode, and where it was fetched from.
- The last up to 8192 executed program counters, oldest first, each tagged ARM or Thumb.

**When filing a bug report, attach the relevant `pokketstation_report_*.log` file.** Include a description of what you were doing right before it happened. This trace is usually the difference between a fixable bug and one that is not.
