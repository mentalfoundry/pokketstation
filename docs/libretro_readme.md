# Libretro frontend

Libretro core for use with [RetroArch](https://www.retroarch.com/).

## Usage

1. Copy `pokketstation_libretro.dll` (`.so` on Linux, `.dylib` on macOS) into RetroArch's **cores** directory.
2. Copy your BIOS dump, **renamed exactly to `pocketstation.bin`**, into RetroArch's **System** directory (Settings → Directory → System/BIOS Directory).
3. In RetroArch: **Load Core** → select **PokketStation** → **Load Content** → select your app file.

The core loads the same three things the desktop app does, picked the same way — by content, not by the file's extension: a full memory-card image (`.mcr`) if the file's size exactly matches the real flash size, otherwise a single-save `.mcs` file, and failing that a bare raw PSX Title Sector dump (`.pss`). A `.gme` whole-card dump still won't work directly, since it wraps the same 128KB of card data in DexDrive's own container header — strip that header down to the raw 128KB first. RetroArch's content browser defaults to showing `.mcr`/`.mcs`/`.pss` files for this core, though the extension itself isn't actually checked at load time.

Controls use RetroArch's standard RetroPad mapping: D-pad for Up/Down/Left/Right, the **A** button for Fire/Action (remappable in RetroArch's own input settings, same as any other core).

The PocketStation's hardware ID (`F_SN`, which sets Final Fantasy VIII Chocobo World's rank) defaults to the best rank, same as the desktop app, but isn't persisted between sessions — there's no config-file mechanism for this core the way the desktop app has. You can still edit this value during a session however.

Single-app loads (`.pss`/`.mcs`) boot through the real BIOS menu the same way a full memory card does — see [Reaching a single loaded app](../README.md#reaching-a-single-loaded-app) in the main README for the button sequence.

## Building

Follow the general build steps in the [desktop frontend's build guide](desktop_readme.md#building) — the core, tools, tests, and this libretro core all build together the same way, with no SDL2/desktop-specific step needed. It fetches `libretro-common` at configure time via CMake's `FetchContent`, so the first configure needs internet access.
