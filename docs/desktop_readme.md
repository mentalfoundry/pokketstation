# Desktop frontend

SDL2-based standalone app for Windows. (Linux app will be forthcoming once the emulator is feature complete).

## Usage

Prebuilt releases ship `pokketstation.exe` directly; building from source instead lands it at `build\Debug\pokketstation.exe` (path depends on your `--config`, see [Building](#building) below). Either way:
```
pokketstation.exe [--console|--no-console] <bios.bin> <app-or-card-file>
```
Example, using a real BIOS dump and a memory-card image (substitute your own paths — neither file is bundled, and **no PocketStation BIOS is bundled** — it's copyrighted Sony firmware, so you need to supply your own dump extracted from real hardware):
```
pokketstation.exe .\bios.bin .\samplememcard.mcr
```
- The second file's **extension doesn't matter** — it's loaded as a full memory-card image (navigate its real BIOS menu with the keyboard, same as real hardware) if its size exactly matches the real flash size; otherwise it's tried as a single-save file with a real PS1 directory frame in front of it (`.mcs` — the format DuckStation, MemcardRex, and most other PS1 save managers export a single save as, and by far the more common of the two in practice) and, failing that, as a bare raw PSX Title Sector app dump (`.pss`). Both `.mcr` card images and single-app loads are confirmed working end-to-end against a real BIOS and real extracted app dumps — see [hardware-notes.md](hardware-notes.md).
- **Double-clicking the .exe directly** (no command line) reuses the last BIOS path remembered from a previous run (see `settings.cfg` below) if there is one, otherwise it falls back to `bios.bin` next to the executable; the app/card side always looks for `memcard.mcr` next to the executable, since that path isn't remembered.
- Launching with no BIOS and/or no app/card present (or an invalid one) isn't fatal — the window still opens; use **File > Load BIOS...** / **File > Open App/Card...** to browse to one instead.
- The app runs without a console window by default (its diagnostic `stderr` output has nowhere to go unless you ask for one). Pass `--console` to get one, or `--no-console` to explicitly suppress it; whichever you pass is remembered in `settings.cfg` for future launches too.
- **Controls:** arrow keys for Up/Down/Left/Right, **Z** for the Fire/Action button by default — remap any of the five from **Tools > Remap Controls...**. The window is freely resizable; **View > Native Size (1x)** / **Double Size (2x)** are just shortcuts back to a known-good size.
- **View > Colors** switches the LCD's rendered look: **Classic** (the default muted LCD-style ink-on-sage look), **Light** (black on white), **Dark** (white on black), or **Custom Colors...** (pick any pixel/background hex pair). **View > Sprite Shadows** toggles a faint one-row "ghosting" trail approximating a real passive-matrix LCD's slow pixel response, with its own configurable color.
- Press **F12** at any time to write a diagnostic report to a log file — see [Diagnostic reports](#diagnostic-reports-for-bug-reports) below.
- The PocketStation's hardware ID (`F_SN`, which sets Final Fantasy VIII Chocobo World's rank) defaults to the best rank and can be viewed/edited from **Tools > Edit Hardware ID...** — see [hardware-notes.md](hardware-notes.md) for the format. This and every other setting above (remembered BIOS path, hardware ID, color scheme, sprite-shadow state, key bindings, `--console`/`--no-console` preference) persist across relaunches in `settings.cfg` next to the executable, written the moment each one actually changes.
- **Help > About pokketstation...** shows the running version and a link back to this repo.

Single-app loads (`.pss`/`.mcs`) boot through the real BIOS menu the same way a full memory card does — see [Reaching a single loaded app](../README.md#reaching-a-single-loaded-app) in the main README for the button sequence.

## Building

### Windows

**Prerequisites:**
- Visual Studio (or the standalone [Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio)) with the **"Desktop development with C++"** workload. This gives you the MSVC compiler and a bundled copy of CMake/Ninja — no separate CMake install needed.
- [vcpkg](https://github.com/microsoft/vcpkg) — needed for this frontend's SDL2 dependency (see below). Not needed if you only want to build the core, tools, tests, or the libretro core.

**1. Open the right terminal.** Regular `cmd`, PowerShell, or Git Bash windows do **not** have the compiler or CMake on `PATH`. From the Start Menu, search for and open **"Developer Command Prompt for VS"** (or "x64 Native Tools Command Prompt for VS") — this is a shortcut that sets up the compiler environment for you automatically. If you can't find that shortcut, open a normal `cmd` window and run this first to set it up manually (adjust the version folder if yours differs from `18`):
```
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%
```

**2. `cd` to the repo.** If the repo is on a different drive than your terminal's current one, plain `cd` won't switch drives — use `/d`:
```
cd /d D:\path\to\pokketstation
```

**3. Configure and build.** CMake's Visual Studio generator is "multi-config" (Debug and Release live side by side), so `--config` needs to be passed consistently to the build and test steps:
```
cmake -B build -S .
cmake --build build --config Debug
ctest --test-dir build -C Debug
```
This builds the core, tools, tests, and the libretro core — but skips this desktop frontend with a warning unless SDL2 is available too (see below).

### Linux / macOS

```
cmake -B build -S .
cmake --build build
ctest --test-dir build
```

### Enabling this frontend (SDL2)

The desktop frontend is skipped with a warning if SDL2 isn't found — the other targets (core, tools, tests) still build fine without it. Install it via vcpkg:
```
vcpkg install sdl2:x64-windows
```
Then **delete your existing `build/` folder and reconfigure from scratch**, pointing CMake at vcpkg's toolchain file — this step matters: CMake caches a "not found" result for SDL2 and won't retry on top of an existing `build/` directory, so reconfiguring in place after installing SDL2 will still fail:
```
rmdir /s /q build
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Debug
```

## Diagnostic reports (for bug reports)

The desktop app can write a detailed diagnostic report to a log file, in two situations:

- **Automatically**, the first time the CPU hits an unrecognized/unimplemented opcode. This is always a real emulator bug, not something you did wrong. The emulator doesn't crash — it stops stepping the CPU and freezes on the last good frame — and prints a message pointing you at the report (only visible if launched with `--console`, see above).
- **On demand, at any time**, by pressing **F12**. Use this for anything that doesn't trip a hard fault but still looks wrong — glitched graphics, missing sound, the app seeming stuck, etc.

Each report is written to the current directory as `psemu_report_<timestamp>.log` (e.g. `psemu_report_20260721_143012.log`) and contains:
- Why it was written (fault vs. manual F12) and which frame number
- The BIOS and app/card file paths you ran with
- Total instructions executed, held button state, `CLK_MODE`, and flash bank-select state (`F_BANK_FLG`/`F_BANK_VAL`)
- All CPU registers, `PC`, and `CPSR` (including ARM vs. Thumb mode)
- If a fault occurred: the exact unrecognized opcode and where it was fetched from
- The last up to 8192 executed program counters (oldest first), each tagged ARM or Thumb

**When filing a bug report, attach the relevant `psemu_report_*.log` file** along with a description of what you were doing right before it happened — that trace is usually the difference between a bug being fixable and not.
