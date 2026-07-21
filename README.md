# pokketstation

An open-source Sony PocketStation emulator core, written in portable C, meant to be reused across a [libretro](https://www.libretro.com/) core, a standalone Windows desktop app, and a PS Vita homebrew port.

## Status

A real PocketStation app (Chocobo World) boots from a real BIOS dump, runs, plays audio, draws its own graphics, and saves without corruption — confirmed across hundreds of millions of instructions of testing with zero unimplemented-opcode hits. The ARM7TDMI interpreter decodes and executes both ARM and Thumb instructions; the memory map, interrupt controller, timers, RTC, flash (including virtual bank-select remapping and real flash-unlock-key protection), DAC/audio, LCD (including `LCD_MODE`/rotation), and the CPU's variable-clock `CLK_MODE` (now using the exact documented per-mode frequency table rather than an approximation) are all wired up and individually verified against real hardware behavior. See [docs/hardware-notes.md](docs/hardware-notes.md) for the full investigation log — what's confirmed, what's still open, and every real bug found and fixed along the way.

**Known gaps:** IR communication timing is unverified, per-instruction cycle timing is still approximate rather than cycle-accurate (see `PSEMU_ASSUMED_CPU_HZ` in `docs/hardware-notes.md`), and a handful of edge cases (low-battery detection, `F_BANK_VAL` entries mapping multiple physical blocks to the same virtual slot, the BIOS's pre-remap boot phase) are deliberately simplified — see the "Confirmed real gaps" list at the end of `docs/hardware-notes.md` for specifics.

## Usage

Prebuilt binaries come in two flavors: a standalone desktop app, and a [libretro](https://www.libretro.com/) core for use with [RetroArch](https://www.retroarch.com/). **Neither bundles a PocketStation BIOS** — it's copyrighted Sony firmware, so either way you need to supply your own dump extracted from real hardware. See [Building](#building) below if you're compiling from source instead of using a prebuilt release.

### Desktop app

```
pokketstation_desktop.exe <bios.bin> <app-or-card-file>
```
- The second file's **extension doesn't matter** — it's loaded as a full memory-card image (navigate its real BIOS menu with the keyboard, same as real hardware) if its size exactly matches the real flash size, or as a single raw PSX Title Sector app dump otherwise.
- **Double-clicking the .exe directly** (no command line) looks for `bios.bin` and `memcard.mcr` in the same folder as the executable and loads those automatically — rename your BIOS dump and memory-card image to those exact names and drop them next to `pokketstation_desktop.exe` for that to work.
- **Controls:** arrow keys for Up/Down/Left/Right, **Z** for the Fire/Action button.
- Press **F12** at any time to write a diagnostic report to a log file — see [Diagnostic reports](#diagnostic-reports-for-bug-reports) below.

### Libretro core (RetroArch)

1. Copy `pokketstation_libretro.dll` (`.so` on Linux, `.dylib` on macOS) into RetroArch's **cores** directory.
2. Copy your BIOS dump, **renamed exactly to `pocketstation.bin`**, into RetroArch's **System** directory (Settings → Directory → System/BIOS Directory).
3. In RetroArch: **Load Core** → select **PokketStation** → **Load Content** → select your app file.

Unlike the desktop app, the libretro core only loads a single raw PSX Title Sector app dump — it does **not** support loading a full memory-card image, so a `.mcr` card dump won't work here even though it works with the desktop app. RetroArch's content browser defaults to showing `.pss` files for this core, though (same as the desktop app) the extension itself isn't actually checked at load time — it's just what the file picker filters to by default.

Controls use RetroArch's standard RetroPad mapping: D-pad for Up/Down/Left/Right, the **A** button for Fire/Action (remappable in RetroArch's own input settings, same as any other core).

## Layout

```
core/                    portable C99 emulation core, no OS/graphics dependencies
  include/psemu/psemu.h  public API
  src/                   CPU, memory map, LCD, buttons, flash, IR
frontends/
  libretro/              libretro core wrapper
  desktop/               SDL2-based Windows/Linux/macOS desktop app
  vita/                  PS Vita port (vita2d), built via the vitasdk toolchain
tests/                   smoke test exercising the public API
docs/hardware-notes.md   hardware reference (memory map, file format, sources)
```

## Building

### Windows

**Prerequisites:**
- Visual Studio (or the standalone [Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio)) with the **"Desktop development with C++"** workload. This gives you the MSVC compiler and a bundled copy of CMake/Ninja — no separate CMake install needed.
- [vcpkg](https://github.com/microsoft/vcpkg) — only required if you want the desktop frontend (see below). Not needed to build just the core/tests.

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

### Linux / macOS

```
cmake -B build -S .
cmake --build build
ctest --test-dir build
```

### Desktop frontend (SDL2)

The desktop frontend is skipped with a warning if SDL2 isn't found — the other targets (core, tools, tests) still build fine without it. To enable it via vcpkg:
```
vcpkg install sdl2:x64-windows
```
Then **delete your existing `build/` folder and reconfigure from scratch**, pointing CMake at vcpkg's toolchain file — this step matters: CMake caches a "not found" result for SDL2 and won't retry on top of an existing `build/` directory, so reconfiguring in place after installing SDL2 will still fail:
```
rmdir /s /q build
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Debug
```
(On Linux/macOS, use vcpkg's own triplet for your platform, or install SDL2 via your system package manager instead and skip the toolchain file.)

Other things worth knowing:
- The libretro core fetches `libretro-common` at configure time via `FetchContent` — this needs internet access on first configure.
- The Vita frontend only builds when configured with the vitasdk toolchain file — see [frontends/vita/README.md](frontends/vita/README.md).

### Running the desktop app

The built executable lands at `build\frontends\desktop\Debug\pokketstation_desktop.exe` (path depends on your `--config`). It takes two required arguments:
```
.\build\frontends\desktop\Debug\pokketstation_desktop.exe <bios.bin> <app-or-card-file>
```
The second argument's file **extension doesn't matter** — the app picks its mode by file size: a file exactly matching the real flash size is loaded as a full memory-card image (`.mcr`, navigate its real BIOS menu with the keyboard, same as real hardware); anything else is loaded as a single raw PSX Title Sector app dump. Confirmed working with `.mcr` card images; single standalone app dumps should work the same way per the code path, but haven't been separately verified.

Example of a known-working invocation, using a real BIOS dump and a memory-card image (substitute your own paths — neither file is bundled):
```
.\build\frontends\desktop\Debug\pokketstation_desktop.exe .\bios.bin .\samplememcard.mcr
```

**No PocketStation BIOS is bundled** — it's copyrighted Sony firmware, so you need to supply your own dump extracted from real hardware, along with an app file or memory-card image to run. The libretro core takes its BIOS the same way, but as `pocketstation.bin` placed in RetroArch's system directory instead of a CLI argument.

### Diagnostic reports (for bug reports)

The desktop app can write a detailed diagnostic report to a log file, in two situations:

- **Automatically**, the first time the CPU hits an unrecognized/unimplemented opcode. This is always a real emulator bug, not something you did wrong. The emulator doesn't crash — it stops stepping the CPU and freezes on the last good frame — and prints a message to the console pointing you at the report.
- **On demand, at any time**, by pressing **F12**. Use this for anything that doesn't trip a hard fault but still looks wrong — glitched graphics, missing sound, the app seeming stuck, etc.

Each report is written to the current directory as `psemu_report_<timestamp>.log` (e.g. `psemu_report_20260721_143012.log`) and contains:
- Why it was written (fault vs. manual F12) and which frame number
- The BIOS and app/card file paths you ran with
- Total instructions executed, held button state, `CLK_MODE`, and flash bank-select state (`F_BANK_FLG`/`F_BANK_VAL`)
- All CPU registers, `PC`, and `CPSR` (including ARM vs. Thumb mode)
- If a fault occurred: the exact unrecognized opcode and where it was fetched from
- The last up to 8192 executed program counters (oldest first), each tagged ARM or Thumb

**When filing a bug report, attach the relevant `psemu_report_*.log` file** along with a description of what you were doing right before it happened — that trace is usually the difference between a bug being fixable and not. These log files are gitignored and never committed to the repo, so don't worry about them showing up in `git status`; just attach them directly to your issue.

## License

GPLv3, see [LICENSE](LICENSE). [docs/hardware-notes.md](docs/hardware-notes.md) has a licensing note on what prior art is safe to reference.
